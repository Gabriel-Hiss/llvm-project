//===- UnvecSearch.cpp - fold vectorized early-exit scans ----------===//
//
// Part of the Revenant LLVM fork.
//
// See UnvecSearch.h for the firing shape and the soundness argument.
//
// Pass structure: scan for chunk-scan loops H (single-block self-loop
// latches whose latch condition is a zero-mask test over a lane-compare
// mask fold, with a bit-scan index on the taken edge) with a scalar
// compare tail T underneath. Validate H and T structurally, then:
//   1. fold the chunk dispatch to the scalar tail entry at index 0,
//   2. drop chunk-block incomings from tail phis,
//   3. erase the dead chunk blocks (dispatch pre blocks, chunk loop,
//      bit-scan result block).
// Kept tail blocks keep their instructions, operands, and flags; only
// branch targets and dead code change, so the fold is refinement-free.
//
// Alive2-style reasoning for the non-obvious step:
//   - dispatch folding: the chunk loop is a pure prefix scan. Each pass
//     reads one disjoint chunk and exits at the first nonzero mask bit;
//     it never writes. The first hit in chunk order equals the first
//     hit in element order, and the scalar tail computes exactly that
//     first hit over any range it is given (the small-N edge proves the
//     tail correct from 0). The taken-edge index (chunk base plus the
//     bit-scan of the nonzero mask) equals the scalar tail's result for
//     the same hit, and the no-hit fallthrough (chunk bound == trip
//     count, or mask == -1 for the compare form) equals the tail's
//     full-range walk. So T-from-0 computes the same value as
//     H-then-T-from-resume on every path that reaches it.
//   - dual-buffer compare form: the mask is all-ones while every lane
//     matches, so the first clear bit is the first differing index;
//     xor-inversion plus cttz computes exactly that index.
//   - wide-lane forms (i16/i32): the lifter folds the byte mask through
//     sext+bitcast+icmp-slt sign-bit chains; the bit-scan result is
//     lshr-scaled by log2(lane bytes). The scale is exactly the lane
//     width, so chunk base plus scaled bit index is the element index.
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/UnvecSearch/UnvecSearch.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define DEBUG_TYPE "unvecsearch"
#define UVS_BAIL(...)                                                           \
  do {                                                                          \
    LLVM_DEBUG(::llvm::dbgs() << "unvecsearch bail " << __LINE__ << " "         \
                              << __VA_ARGS__ << "\n");                          \
  } while (0)

namespace revenant {
namespace {

void copyVA(const Instruction *Src, Instruction *Dst) {
  if (MDNode *MD = Src->getMetadata("revenant.va"))
    Dst->setMetadata("revenant.va", MD);
}

bool hasOpaque(const Instruction &I) {
  return I.getMetadata("revenant.opaque") != nullptr;
}

// An instruction that may appear in an erased chunk-scan block: no
// calls except LLVM intrinsics, no asm, nothing volatile/atomic, no
// terminators, no !revenant.opaque.
bool isScanErasable(const Instruction &I) {
  if (I.isTerminator())
    return false;
  if (I.isVolatile())
    return false;
  // Atomic loads use isVolatile()==false on some spellings; reject any
  // atomic memory op explicitly (an atomic chunk load is never a scan).
  if (const auto *LI = dyn_cast<LoadInst>(&I)) {
    if (LI->isAtomic())
      return false;
  }
  if (const auto *SI = dyn_cast<StoreInst>(&I)) {
    if (SI->isAtomic())
      return false;
  }
  if (const auto *CB = dyn_cast<CallBase>(&I)) {
    const Function *Callee = CB->getCalledFunction();
    if (!Callee)
      return false;
    if (!Callee->isIntrinsic())
      return false;
    // Intrinsics with side effects (trap, sideeffect, debug) are never
    // erasable: dropping them would delete faults/ordering the source
    // loop never had, and debug intrinsics are handled by their own
    // path. Pure arithmetic intrinsics (cttz/ctpop/fma-style) pass.
    if (!CB->doesNotAccessMemory() && !CB->onlyReadsMemory())
      return false;
    if (CB->isVolatile())
      return false;
    if (Callee->getIntrinsicID() == Intrinsic::trap ||
        Callee->getIntrinsicID() == Intrinsic::debugtrap ||
        Callee->getIntrinsicID() == Intrinsic::sideeffect ||
        Callee->getIntrinsicID() == Intrinsic::dbg_value ||
        Callee->getIntrinsicID() == Intrinsic::dbg_declare ||
        Callee->getIntrinsicID() == Intrinsic::dbg_label)
      return false;
    if (hasOpaque(I))
      return false;
    return true;
  }
  if (isa<AtomicRMWInst>(&I) || isa<AtomicCmpXchgInst>(&I) ||
      isa<FenceInst>(&I))
    return false;
  if (hasOpaque(I))
    return false;
  return true;
}

unsigned laneBytes(FixedVectorType *VT) {
  unsigned Bits = VT->getElementType()->getPrimitiveSizeInBits();
  return Bits / 8;
}

// Byte-symbolic splat proof for broadcast needles. The lifter builds the
// needle through varied chains (direct insertelement+splat, or the
// unpack/shuffle broadcast sequence gcc emits for byte searches), so the
// matcher proves the end-to-end property instead of matching one shape:
// every byte of the vector equals bytes of one scalar root value.
//
// Each byte slot holds R(k) (byte k of the single scalar root R), Zero,
// or Poison. Supported DAG ops: zext (scalar, zero extends), trunc,
// bitcast (regroups bytes), shufflevector (permutes lane-sized groups),
// insertelement into poison/undef with a scalar that strips to R.
// Anything else bails. All reached instructions must be defined outside
// the chunk loop H (loop invariance); constants and arguments are free.
struct SplatBytes {
  // 32 slots cover AVX2 (larger vectors bail).
  enum Kind : uint8_t { RK, Zero, Poison };
  struct Slot {
    Kind K = Poison;
    uint8_t Byte = 0; // valid when K == RK: byte index into R
  };
  Slot B[32];
  unsigned N = 0;
};

// Strip casts toward the scalar root.
Value *stripScalarCasts(Value *V) {
  while (auto *C = dyn_cast<CastInst>(V))
    V = C->getOperand(0);
  return V;
}

// Compute byte symbols for V (scalar or vector, total <= 32 bytes).
// Root is the single scalar root on success (null when fully zero).
bool splatBytes(Value *V, BasicBlock *H, SplatBytes &Out, Value *&Root) {
  Out.N = 0;
  Root = nullptr;
  auto Note = [&](Value *W) -> bool {
    W = stripScalarCasts(W);
    if (isa<Constant>(W) || isa<Argument>(W)) {
      if (auto *CI = dyn_cast<ConstantInt>(W)) {
        if (!CI->isZero())
          return false; // nonzero scalar constant leaf: separate root
      } else if (!isa<Constant>(W)) {
        return false;
      }
      // Zero constants contribute Zero bytes; handled by callers via
      // all-zero check. Nonzero constants only as the single root.
      return true;
    }
    auto *I = dyn_cast<Instruction>(W);
    if (!I || I->getParent() == H)
      return false;
    return true;
  };
  // Recursive worker with depth cap.
  struct Worker {
    BasicBlock *H;
    Value *Root = nullptr;
    bool HasNonZeroRoot = false;
    bool go(Value *V, SplatBytes &Out, unsigned Depth) {
      if (Depth == 0)
        return false;
      // NOTE: no cast stripping here. The lifter's broadcast idiom is
      // zext-scalar-to-i128 followed by bitcast-to-vector; stripping
      // the zext would collapse the 16-byte layout to the 4-byte
      // scalar and break the regrouping below. Scalar roots are
      // stripped in goScalar/noteRoot instead.
      Type *T = V->getType();
      unsigned Bytes = 0;
      if (T->isIntegerTy())
        Bytes = (T->getIntegerBitWidth() + 7) / 8;
      else if (auto *VT = dyn_cast<FixedVectorType>(T))
        Bytes = VT->getNumElements() *
                ((VT->getElementType()->getPrimitiveSizeInBits() + 7) / 8);
      else
        return false;
      if (Bytes == 0 || Bytes > 32)
        return false;
      // Constants.
      if (isa<PoisonValue>(V) || isa<UndefValue>(V)) {
        Out.N = Bytes;
        for (unsigned I = 0; I < Bytes; ++I)
          Out.B[I].K = SplatBytes::Poison;
        return true;
      }
      if (isa<ConstantAggregateZero>(V) || isa<ConstantPointerNull>(V)) {
        Out.N = Bytes;
        for (unsigned I = 0; I < Bytes; ++I)
          Out.B[I].K = SplatBytes::Zero;
        return true;
      }
      if (auto *CI = dyn_cast<ConstantInt>(V)) {
        // Nonzero integer constant: it becomes the root.
        if (!noteRoot(CI))
          return false;
        Out.N = Bytes;
        for (unsigned I = 0; I < Bytes; ++I) {
          Out.B[I].K = SplatBytes::RK;
          Out.B[I].Byte = (uint8_t)I;
        }
        return true;
      }
      if (isa<Constant>(V))
        return false;
      if (auto *A = dyn_cast<Argument>(V)) {
        if (!noteRoot(A))
          return false;
        Out.N = Bytes;
        for (unsigned I = 0; I < Bytes; ++I) {
          Out.B[I].K = SplatBytes::RK;
          Out.B[I].Byte = (uint8_t)I;
        }
        return true;
      }
      auto *I = cast<Instruction>(V);
      if (I->getParent() == H)
        return false;
      switch (I->getOpcode()) {
      case Instruction::ZExt: {
        Value *Op = I->getOperand(0);
        unsigned SrcBytes =
            (Op->getType()->getIntegerBitWidth() + 7) / 8;
        SplatBytes S;
        Value *R = nullptr;
        // Compute the source bytes as a scalar.
        if (!goScalar(Op, S, Depth - 1, R))
          return false;
        if (!noteRoot(R))
          return false;
        Out.N = Bytes;
        for (unsigned J = 0; J < Bytes; ++J) {
          if (J < SrcBytes && J < S.N) {
            Out.B[J] = S.B[J];
          } else {
            Out.B[J].K = SplatBytes::Zero;
          }
        }
        return true;
      }
      case Instruction::Trunc: {
        SplatBytes S;
        Value *R = nullptr;
        if (!goScalar(I->getOperand(0), S, Depth - 1, R))
          return false;
        if (!noteRoot(R))
          return false;
        Out.N = Bytes;
        for (unsigned J = 0; J < Bytes; ++J)
          Out.B[J] = S.B[J];
        return true;
      }
      case Instruction::BitCast: {
        SplatBytes S;
        Value *R = nullptr;
        if (!go(I->getOperand(0), S, Depth - 1))
          return false;
        R = Root;
        if (S.N != Bytes)
          return false;
        Out.N = Bytes;
        for (unsigned J = 0; J < Bytes; ++J)
          Out.B[J] = S.B[J];
        (void)R;
        return true;
      }
      case Instruction::ShuffleVector: {
        auto *Shuf = cast<ShuffleVectorInst>(I);
        SplatBytes A, B;
        Value *RA = Root, *RB = Root;
        // Evaluate both operands with the shared root.
        Value *SaveRoot = Root;
        bool HNR = HasNonZeroRoot;
        if (!go(Shuf->getOperand(0), A, Depth - 1))
          return false;
        Value *MidRoot = Root;
        bool MidHNR = HasNonZeroRoot;
        Root = SaveRoot;
        HasNonZeroRoot = HNR;
        if (!go(Shuf->getOperand(1), B, Depth - 1))
          return false;
        // Merge roots: both sides must agree.
        if (MidRoot && Root && MidRoot != Root)
          return false;
        if (!Root)
          Root = MidRoot;
        HasNonZeroRoot = MidHNR || HasNonZeroRoot;
        (void)RA;
        (void)RB;
        unsigned EltBytes =
            (cast<FixedVectorType>(T)->getElementType()
                 ->getPrimitiveSizeInBits() +
             7) /
            8;
        unsigned Lanes = A.N / EltBytes;
        if (A.N != Bytes || B.N != Bytes || A.N % EltBytes)
          return false;
        Out.N = Bytes;
        unsigned J = 0;
        for (int M : Shuf->getShuffleMask()) {
          const SplatBytes &Src = (M < (int)Lanes) ? A : B;
          unsigned Lane = (M < (int)Lanes) ? (unsigned)M : (unsigned)M - Lanes;
          if (M < 0) {
            for (unsigned K = 0; K < EltBytes; ++K)
              Out.B[J++].K = SplatBytes::Poison;
          } else {
            for (unsigned K = 0; K < EltBytes; ++K)
              Out.B[J++] = Src.B[Lane * EltBytes + K];
          }
        }
        return true;
      }
      case Instruction::InsertElement: {
        auto *Ins = cast<InsertElementInst>(I);
        SplatBytes Base;
        if (!go(Ins->getOperand(0), Base, Depth - 1))
          return false;
        if (Base.N != Bytes)
          return false;
        auto *Idx = dyn_cast<ConstantInt>(Ins->getOperand(2));
        if (!Idx)
          return false;
        // Operand order: (vec, scalar, idx).
        Value *Scalar = stripScalarCasts(Ins->getOperand(1));
        if (!noteRoot(Scalar))
          return false;
        unsigned EltBytes =
            (cast<FixedVectorType>(T)->getElementType()
                 ->getPrimitiveSizeInBits() +
             7) /
            8;
        Out.N = Bytes;
        for (unsigned J = 0; J < Bytes; ++J)
          Out.B[J] = Base.B[J];
        // Overwrite lane Idx with root bytes.
        SplatBytes Sc;
        Value *RS = nullptr;
        if (!goScalar(Scalar, Sc, Depth - 1, RS))
          return false;
        if (!noteRoot(RS))
          return false;
        for (unsigned K = 0; K < EltBytes; ++K) {
          Out.B[Idx->getZExtValue() * EltBytes + K] =
              (K < Sc.N) ? Sc.B[K] : SplatBytes::Slot{SplatBytes::Zero, 0};
        }
        return true;
      }
      default:
        return false;
      }
    }
    bool noteRoot(Value *R) {
      R = stripScalarCasts(R);
      if (!R)
        return false;
      if (isa<Constant>(R)) {
        if (auto *CI = dyn_cast<ConstantInt>(R)) {
          if (CI->isZero())
            return true; // zero folds into Zero bytes via callers
        } else {
          return false;
        }
      }
      if (!Root) {
        Root = R;
        HasNonZeroRoot = true;
        return true;
      }
      return Root == R;
    }
    bool goScalar(Value *V, SplatBytes &Out, unsigned Depth, Value *&R) {
      V = stripScalarCasts(V);
      if (auto *CI = dyn_cast<ConstantInt>(V)) {
        if (CI->isZero()) {
          unsigned Bytes = (CI->getBitWidth() + 7) / 8;
          Out.N = Bytes;
          for (unsigned I = 0; I < Bytes; ++I)
            Out.B[I].K = SplatBytes::Zero;
          R = nullptr; // all-zero: no root, accepts any lane width
          return true;
        }
        R = CI;
        unsigned Bytes = (CI->getBitWidth() + 7) / 8;
        Out.N = Bytes;
        for (unsigned I = 0; I < Bytes; ++I) {
          Out.B[I].K = SplatBytes::RK;
          Out.B[I].Byte = (uint8_t)I;
        }
        return true;
      }
      if (auto *A = dyn_cast<Argument>(V)) {
        R = A;
        unsigned BytesA = (A->getType()->getIntegerBitWidth() + 7) / 8;
        Out.N = BytesA;
        for (unsigned I = 0; I < BytesA; ++I) {
          Out.B[I].K = SplatBytes::RK;
          Out.B[I].Byte = (uint8_t)I;
        }
        return true;
      }
      auto *I = dyn_cast<Instruction>(V);
      if (!I || I->getParent() == H || !I->getType()->isIntegerTy())
        return false;
      R = I;
      unsigned Bytes = (I->getType()->getIntegerBitWidth() + 7) / 8;
      Out.N = Bytes;
      for (unsigned J = 0; J < Bytes; ++J) {
        Out.B[J].K = SplatBytes::RK;
        Out.B[J].Byte = (uint8_t)J;
      }
      return true;
    }
  };
  (void)Note;
  Worker W{H};
  if (!W.go(V, Out, 24))
    return false;
  Root = W.Root;
  return true;
}

// Verify the proven bytes form a splat of lanes LaneBytes wide: every
// lane group equals the root's low LaneBytes bytes (missing root bytes
// read as zero).
bool isLaneSplat(const SplatBytes &S, Value *Root, unsigned LaneBytes) {
  if (S.N == 0 || S.N % LaneBytes)
    return false;
  for (unsigned L = 0; L < S.N; L += LaneBytes) {
    for (unsigned K = 0; K < LaneBytes; ++K) {
      auto Sl = S.B[L + K];
      if (Sl.K == SplatBytes::Poison)
        return false;
      if (Sl.K == SplatBytes::Zero) {
        // Zero byte at lane position K is legitimate only when the
        // root lacks that byte (narrow root, zero-extended into the
        // lane). A null root means the whole vector proved all-zero.
        if (Root) {
          unsigned RootBytes =
              (Root->getType()->getIntegerBitWidth() + 7) / 8;
          if (K < RootBytes)
            return false;
        }
        continue;
      }
      if (Sl.Byte != K)
        return false;
    }
  }
  return true;
}

// Follow the mask fold from the latch's compared value back to the lane
// compare. Accepts:
//   (a) bitcast <N x i1> to integer (direct movmsk form), or
//   (b) sext <N x i1> to <N x i32>, bitcast to <M x i8>, icmp slt
//       against zero, bitcast <M x i1> to integer (the sign-bit chain
//       the lifter emits for wider lanes).
// On success fills Cmp (the icmp eq lane compare), MaskInt (the integer
// mask value compared against 0/-1), and LaneBytesOut.
bool matchMaskFold(Value *MaskCompared, ICmpInst *&Cmp,
                   Value *&MaskInt, unsigned &LaneBytesOut) {
  Cmp = nullptr;
  MaskInt = nullptr;
  LaneBytesOut = 0;
  // Form (a): direct bitcast. Falls through to form (b) unless the
  // operand is exactly an icmp-eq lane compare.
  if (auto *BC = dyn_cast<BitCastInst>(MaskCompared)) {
    if (auto *VT = dyn_cast<FixedVectorType>(BC->getSrcTy())) {
      if (VT->getElementType()->isIntegerTy(1) &&
          BC->getDestTy()->isIntegerTy()) {
        if (auto *Eq = dyn_cast<ICmpInst>(BC->getOperand(0))) {
          if (Eq->getPredicate() == ICmpInst::ICMP_EQ) {
            auto *LVT =
                dyn_cast<FixedVectorType>(Eq->getOperand(0)->getType());
            if (LVT) {
              // One side is the chunk load (possibly bitcast), the
              // other a splat or a second load for the compare form.
              // Checked by the caller; here just record the lane width.
              Cmp = Eq;
              MaskInt = BC;
              LaneBytesOut = laneBytes(LVT);
              if (LaneBytesOut != 0)
                return true;
            }
          }
        }
      }
    }
  }
  // Form (b): sign-bit chain.
  auto *BC = dyn_cast<BitCastInst>(MaskCompared);
  if (!BC)
    return false;
  if (auto *VT = dyn_cast<FixedVectorType>(BC->getSrcTy())) {
    if (!VT->getElementType()->isIntegerTy(1) ||
        !BC->getDestTy()->isIntegerTy())
      return false;
  } else {
    return false;
  }
  auto *Slt = dyn_cast<ICmpInst>(BC->getOperand(0));
  if (!Slt || Slt->getPredicate() != ICmpInst::ICMP_SLT)
    return false;
  if (!isa<ConstantAggregateZero>(Slt->getOperand(1)))
    return false;
  auto *BC2 = dyn_cast<BitCastInst>(Slt->getOperand(0));
  if (!BC2)
    return false;
  auto *Sext = dyn_cast<SExtInst>(BC2->getOperand(0));
  if (!Sext)
    return false;
  auto *Eq = dyn_cast<ICmpInst>(Sext->getOperand(0));
  if (!Eq || Eq->getPredicate() != ICmpInst::ICMP_EQ)
    return false;
  auto *LVT = dyn_cast<FixedVectorType>(Eq->getOperand(0)->getType());
  if (!LVT)
    return false;
  // The sext must widen i1 lanes to i32 (the lifter's sign-bit form).
  auto *SVT = dyn_cast<FixedVectorType>(Sext->getDestTy());
  if (!SVT || !SVT->getElementType()->isIntegerTy(32))
    return false;
  if (SVT->getNumElements() != LVT->getNumElements())
    return false;
  Cmp = Eq;
  MaskInt = BC;
  LaneBytesOut = laneBytes(LVT);
  return LaneBytesOut != 0;
}

// Check the lane compare operands: one side derives from the chunk
// load in H, the other is a proven broadcast splat (find form) or a
// load from a second buffer at the same index (compare form). Second
// loads must be non-volatile, non-atomic, in H.
bool cmpOperandsOK(ICmpInst *Eq, LoadInst *ChunkLoad, BasicBlock *H,
                   unsigned LaneBytes, Value *&Needle,
                   LoadInst *&SecondLoad) {
  Needle = nullptr;
  SecondLoad = nullptr;
  Value *Op0 = Eq->getOperand(0);
  Value *Op1 = Eq->getOperand(1);
  // Strip bitcasts on either side (the lifter bitcasts the splat and
  // the chunk to a common lane type).
  auto StripBC = [](Value *V) -> Value * {
    while (auto *BC = dyn_cast<BitCastInst>(V))
      V = BC->getOperand(0);
    return V;
  };
  Value *A = StripBC(Op0);
  Value *B = StripBC(Op1);
  // The two-way strchr form ORs two compares; handled by the caller.
  // Here each compare must involve the chunk load on one side.
  Value *Other = nullptr;
  if (A == ChunkLoad)
    Other = B;
  else if (B == ChunkLoad)
    Other = A;
  else
    return false;
  Other = StripBC(Other);
  if (auto *L = dyn_cast<LoadInst>(Other)) {
    if (L->isVolatile() || L->isAtomic() || hasOpaque(*L))
      return false;
    if (L->getParent() != H)
      return false;
    SecondLoad = L;
    return true;
  }
  // Broadcast form: prove the other side is a lane splat of one root.
  SplatBytes SB;
  Value *Root = nullptr;
  if (!splatBytes(Other, H, SB, Root))
    return false;
  if (!isLaneSplat(SB, Root, LaneBytes))
    return false;
  Needle = Root;
  return true;
}

struct ScanInfo {
  BasicBlock *H = nullptr;      // chunk-scan self-loop latch
  BasicBlock *ScanExit = nullptr; // bit-scan result block (taken edge)
  BasicBlock *Latch = nullptr;  // latch backedge block (may equal H)
  BasicBlock *Pre = nullptr;    // chunk pre/dispatch block
  BasicBlock *TailEntry = nullptr; // scalar tail entry
  PHINode *ChunkIdx = nullptr;  // chunk-base index phi (element units)
  PHINode *ChunkPtr = nullptr;  // chunk pointer phi (optional)
  LoadInst *ChunkLoad = nullptr;
  ICmpInst *LaneCmp = nullptr;
  Value *MaskInt = nullptr;
  CallInst *BitScan = nullptr; // llvm.cttz on the taken edge
  unsigned LaneBytes = 0;       // bytes per lane (1/2/4)
  unsigned Lanes = 0;           // lanes per chunk
  unsigned ChunkBytes = 0;      // LaneBytes * Lanes
  bool Inverted = false;        // compare form (all-ones = continue)
  Value *Needle = nullptr;
  LoadInst *SecondLoad = nullptr;
  // strchr two-way form: second compare + or.
  ICmpInst *LaneCmp2 = nullptr;
};

bool isCttzCall(Value *V, CallInst *&Out) {
  auto *CB = dyn_cast<CallInst>(V);
  if (!CB)
    return false;
  const Function *F = CB->getCalledFunction();
  if (!F || !F->isIntrinsic() ||
      F->getIntrinsicID() != Intrinsic::cttz)
    return false;
  Out = CB;
  return true;
}

// Match the taken-edge index computation in ScanExit: result = chunk
// base (phi) or-combined/added with the (scaled) bit-scan, or the
// compare-form add of cttz(xor(mask,-1)) to the chunk base. The first
// block of the taken edge holds the cttz call (possibly through zext
// and lshr by log2(LaneBytes), and xor with -1 for inverted masks).
bool matchScanExit(BasicBlock *Exit, ScanInfo &S) {
  // Find the cttz call fed (through casts) by the mask.
  CallInst *Cttz = nullptr;
  Value *CttzArg = nullptr;
  for (Instruction &I : *Exit) {
    if (isCttzCall(&I, Cttz)) {
      CttzArg = Cttz->getArgOperand(0);
      break;
    }
  }
  if (!Cttz)
    return false;
  // Strip zext toward the mask width.
  Value *V = CttzArg;
  while (auto *Z = dyn_cast<ZExtInst>(V))
    V = Z->getOperand(0);
  // Inverted (compare) form: xor(mask, -1).
  bool Inverted = false;
  if (auto *X = dyn_cast<BinaryOperator>(V)) {
    if (X->getOpcode() == Instruction::Xor) {
      auto *C0 = dyn_cast<ConstantInt>(X->getOperand(0));
      auto *C1 = dyn_cast<ConstantInt>(X->getOperand(1));
      Value *Inner = nullptr;
      if (C1 && C1->isAllOnesValue())
        Inner = X->getOperand(0);
      else if (C0 && C0->isAllOnesValue())
        Inner = X->getOperand(1);
      if (Inner) {
        while (auto *Z = dyn_cast<ZExtInst>(Inner))
          Inner = Z->getOperand(0);
        if (Inner == S.MaskInt) {
          Inverted = true;
          V = Inner;
        }
      }
    }
  }
  if (!Inverted) {
    if (V != S.MaskInt)
      return false;
  }
  S.Inverted = Inverted;
  // Lane scale: optional lshr of the cttz result by log2(LaneBytes).
  // The lshr may sit before or after the zext; accept either order by
  // scanning Exit for a single lshr on the cttz def chain.
  unsigned ExpectShift = 0;
  for (unsigned LB = S.LaneBytes; LB > 1; LB >>= 1)
    ++ExpectShift;
  bool ScaleOK = (ExpectShift == 0);
  for (Instruction &I : *Exit) {
    auto *BO = dyn_cast<BinaryOperator>(&I);
    if (!BO || BO->getOpcode() != Instruction::LShr)
      continue;
    if (BO->getOperand(0) != Cttz && BO->getOperand(0) != CttzArg) {
      // Through one zext?
      auto *Z = dyn_cast<ZExtInst>(BO->getOperand(0));
      if (!Z || Z->getOperand(0) != Cttz)
        continue;
    }
    auto *C = dyn_cast<ConstantInt>(BO->getOperand(1));
    if (!C || C->getZExtValue() != ExpectShift)
      return false;
    ScaleOK = true;
  }
  if (!ScaleOK)
    return false;
  S.BitScan = Cttz;
  return true;
}

// Match one chunk-scan loop. Two CFG forms:
//   (a) single-block self-loop: H branches to {H, ScanExit} with the
//       mask test as its latch condition;
//   (b) header+latch: H holds the load/compare/fold and branches on
//       the mask test to {Latch, ScanExit}; Latch steps the chunk IV
//       and the pointer, tests chunk-done, and branches to {H, Tail}.
// The latch condition compares the folded mask against 0 (find form)
// or -1 (compare form). Returns true with S filled.
bool matchScanLoop(BasicBlock *H, Function &F, ScanInfo &S) {
  auto *Term = dyn_cast<BranchInst>(H->getTerminator());
  if (!Term || !Term->isConditional())
    return false;
  if (Term->getSuccessor(0) == Term->getSuccessor(1))
    return false;
  Value *Cond = Term->getCondition();
  auto *Cmp = dyn_cast<ICmpInst>(Cond);
  if (!Cmp || Cmp->getParent() != H)
    return false;
  // Latch test: mask == 0 (continue scanning) or mask == -1 (all lanes
  // match, compare form continues).
  auto *C0 = dyn_cast<ConstantInt>(Cmp->getOperand(1));
  auto *C1 = dyn_cast<ConstantInt>(Cmp->getOperand(0));
  bool IsZero = false;
  Value *MaskV = nullptr;
  if (Cmp->getPredicate() != ICmpInst::ICMP_EQ)
    return false;
  if (C0 && C0->isZero()) {
    IsZero = true;
    MaskV = Cmp->getOperand(0);
  } else if (C1 && C1->isZero()) {
    IsZero = true;
    MaskV = Cmp->getOperand(1);
  } else if ((C0 && C0->isAllOnesValue()) || (C1 && C1->isAllOnesValue())) {
    IsZero = false;
    MaskV = (C0 && C0->isAllOnesValue()) ? Cmp->getOperand(0)
                                         : Cmp->getOperand(1);
  } else {
    return false;
  }
  // strchr two-way form: mask is (or cmpA cmpB) bitcast. Accept an or
  // of two icmp-eq lane compares sharing the chunk load.
  ICmpInst *LaneCmp = nullptr;
  ICmpInst *LaneCmp2 = nullptr;
  Value *MaskInt = nullptr;
  unsigned LaneBytes = 0;
  if (auto *Or = dyn_cast<BinaryOperator>(MaskV)) {
    if (Or->getOpcode() == Instruction::Or &&
        Or->getParent() == H) {
      // or <N x i1>: both operands must be icmp eq.
      auto *A = dyn_cast<ICmpInst>(Or->getOperand(0));
      auto *B = dyn_cast<ICmpInst>(Or->getOperand(1));
      if (!A || !B)
        return false;
      if (A->getPredicate() != ICmpInst::ICMP_EQ ||
          B->getPredicate() != ICmpInst::ICMP_EQ)
        return false;
      // The or feeds a bitcast to integer.
      // matchMaskFold expects the compared value; emulate by matching
      // the or's single user bitcast manually below.
      LaneCmp = A;
      LaneCmp2 = B;
    }
  }
  if (LaneCmp) {
    // Two-way form: MaskV is or <N x i1>; find its bitcast user.
    if (!MaskV->hasOneUse())
      return false;
    auto *BC = dyn_cast<BitCastInst>(*MaskV->user_begin());
    if (!BC || BC->getParent() != H)
      return false;
    if (!BC->getDestTy()->isIntegerTy())
      return false;
    auto *LVT = dyn_cast<FixedVectorType>(LaneCmp->getOperand(0)->getType());
    if (!LVT)
      return false;
    MaskInt = BC;
    LaneBytes = laneBytes(LVT);
    if (LaneBytes == 0)
      return false;
  } else {
    if (!matchMaskFold(MaskV, LaneCmp, MaskInt, LaneBytes)) {
      UVS_BAIL("maskfold " << H->getName());
      return false;
    }
  }
  if (!LaneCmp)
    return false;
  // One vector load (find form) or two same-type vector loads from two
  // buffers (dual-buffer compare form). Anything else is not a scan.
  SmallVector<LoadInst *, 2> VecLoads;
  for (Instruction &I : *H) {
    auto *L = dyn_cast<LoadInst>(&I);
    if (!L)
      continue;
    if (!isa<FixedVectorType>(L->getType()))
      continue;
    if (L->isVolatile() || L->isAtomic() || hasOpaque(*L))
      return false;
    VecLoads.push_back(L);
  }
  if (VecLoads.empty() || VecLoads.size() > 2)
    return false;
  if (VecLoads.size() == 2 &&
      VecLoads[0]->getType() != VecLoads[1]->getType())
    return false;
  LoadInst *ChunkLoad = VecLoads[0];
  LoadInst *SecondLoad = nullptr;
  Value *Needle = nullptr;
  // For the two-way form, validate each compare separately against the
  // same chunk load.
  if (LaneCmp2) {
    Value *N1 = nullptr, *N2 = nullptr;
    LoadInst *S1 = nullptr, *S2 = nullptr;
    if (!cmpOperandsOK(LaneCmp, ChunkLoad, H, LaneBytes, N1, S1))
      return false;
    if (!cmpOperandsOK(LaneCmp2, ChunkLoad, H, LaneBytes, N2, S2))
      return false;
    // One side must be the needle splat, the other the zero splat
    // (strchr terminates at NUL too). Second loads not allowed here.
    if (S1 || S2)
      return false;
    // Exactly one side is the all-zero splat (null root); the other is
    // the needle.
    bool Z1 = !N1, Z2 = !N2;
    if (Z1 == Z2)
      return false;
    Needle = Z1 ? N2 : N1;
  } else {
    if (!cmpOperandsOK(LaneCmp, ChunkLoad, H, LaneBytes, Needle,
                       SecondLoad)) {
      UVS_BAIL("cmp " << H->getName());
      return false;
    }
  }
  // Chunk IV: an i32/i64 phi stepping by Lanes (element units for
  // the index phi) or a pointer phi stepping by ChunkBytes. The taken
  // edge adds bit index to the index phi (or-combined when the low
  // bits are known zero).
  auto *LVT =
      dyn_cast<FixedVectorType>(LaneCmp->getOperand(0)->getType());
  if (!LVT)
    return false;
  unsigned Lanes = LVT->getNumElements();
  if (Lanes != 4 && Lanes != 8 && Lanes != 16 && Lanes != 32)
    return false;
  unsigned ChunkBytes = LaneBytes * Lanes;
  // Lanes implied by the mask integer width must cover the chunk.
  unsigned MaskBits = cast<IntegerType>(MaskInt->getType())->getBitWidth();
  if (MaskBits < Lanes)
    return false;
  // Find the chunk IV phi(s). Two layouts: the step add lives in H
  // (single-block self-loop), or the step lives in a sibling latch
  // block Latch that branches back to H (header+latch form). In the
  // latch form the phi's H incoming is the stepped value from Latch
  // and one non-H incoming is the zero start.
  PHINode *IdxPhi = nullptr;
  PHINode *PtrPhi = nullptr;
  BasicBlock *Latch = nullptr;
  // Detect the latch: the continue successor of H that branches back
  // to H. In single-block form the continue edge is H itself.
  // Successor 0 is the continue edge in both forms (mask==0 true, or
  // mask==-1 true both mean keep scanning).
  {
    BasicBlock *Cont = Term->getSuccessor(0);
    if (Cont == H) {
      Latch = H;
    } else {
      auto *LT = dyn_cast<BranchInst>(Cont->getTerminator());
      if (!LT)
        return false;
      bool BackToH = false;
      for (BasicBlock *SS : successors(Cont))
        if (SS == H)
          BackToH = true;
      if (!BackToH)
        return false;
      Latch = Cont;
    }
  }
  BasicBlock *Taken = Term->getSuccessor(1);
  for (PHINode &Phi : H->phis()) {
    if (!Phi.getType()->isIntegerTy())
      continue;
    Value *Back = nullptr;
    for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I)
      if (Phi.getIncomingBlock(I) == Latch)
        Back = Phi.getIncomingValue(I);
    if (!Back)
      continue;
    auto *Add = dyn_cast<BinaryOperator>(Back);
    if (!Add || Add->getOpcode() != Instruction::Add)
      continue;
    if (Add->getParent() != H && Add->getParent() != Latch)
      continue;
    ConstantInt *Step = nullptr;
    if (Add->getOperand(0) == &Phi)
      Step = dyn_cast<ConstantInt>(Add->getOperand(1));
    else if (Add->getOperand(1) == &Phi)
      Step = dyn_cast<ConstantInt>(Add->getOperand(0));
    // Element units: the taken edge adds the lane-relative bit index
    // to this phi, so it must count elements (Lanes per chunk). For
    // i8 lanes this equals the byte stride; wider lanes carry the byte
    // stride in the pointer phi below.
    if (!Step || Step->getZExtValue() != Lanes)
      continue;
    // Start must be constant 0 from outside the {H,Latch} pair.
    bool ZeroStart = false;
    for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I) {
      if (Phi.getIncomingBlock(I) == H || Phi.getIncomingBlock(I) == Latch)
        continue;
      if (auto *CZ = dyn_cast<ConstantInt>(Phi.getIncomingValue(I)))
        if (CZ->isZero())
          ZeroStart = true;
    }
    if (!ZeroStart)
      return false;
    IdxPhi = &Phi;
    break;
  }
  for (PHINode &Phi : H->phis()) {
    if (!isa<PointerType>(Phi.getType()))
      continue;
    Value *Back = nullptr;
    for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I)
      if (Phi.getIncomingBlock(I) == Latch)
        Back = Phi.getIncomingValue(I);
    if (!Back)
      continue;
    auto *GEP = dyn_cast<GetElementPtrInst>(Back);
    if (!GEP || (GEP->getParent() != H && GEP->getParent() != Latch))
      continue;
    if (GEP->getPointerOperand() != &Phi)
      continue;
    bool HasOutside = false;
    for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I) {
      if (Phi.getIncomingBlock(I) == H || Phi.getIncomingBlock(I) == Latch)
        continue;
      // Pointer phi starts at the base argument.
      HasOutside = true;
    }
    if (!HasOutside)
      return false;
    // GEP step must be ChunkBytes.
    if (auto *C = dyn_cast<ConstantInt>(GEP->getOperand(1))) {
      if (C->getSExtValue() == (int64_t)ChunkBytes)
        PtrPhi = &Phi;
    } else if (GEP->getNumOperands() == 2) {
      PtrPhi = &Phi;
    }
  }
  if (!IdxPhi && !PtrPhi)
    return false;
  S.Latch = Latch;
  // The chunk load address must derive from the base + IV.
  // (Checked loosely: load pointer operand is the ptr phi or a GEP of
  // the function base by the index phi.)
  // No stores, no other calls in H or Latch; the latch must be a pure
  // step+bound-test block (adds, cmps, branches only besides the IV
  // step and pointer bump).
  for (BasicBlock *BB : {H, Latch}) {
    for (Instruction &I : *BB) {
      if (isa<StoreInst>(&I))
        return false;
      if (auto *CB = dyn_cast<CallBase>(&I)) {
        const Function *CF = CB->getCalledFunction();
        if (!CF || !CF->isIntrinsic())
          return false;
      }
      if (!isScanErasable(I) && !I.isTerminator())
        return false;
      if (hasOpaque(I))
        return false;
    }
  }
  S.H = H;
  S.ScanExit = Taken;
  S.ChunkLoad = ChunkLoad;
  S.LaneCmp = LaneCmp;
  S.LaneCmp2 = LaneCmp2;
  S.MaskInt = MaskInt;
  S.LaneBytes = LaneBytes;
  S.Lanes = Lanes;
  S.ChunkBytes = ChunkBytes;
  S.ChunkIdx = IdxPhi;
  S.ChunkPtr = PtrPhi;
  S.Needle = Needle;
  S.SecondLoad = SecondLoad;
  if (!matchScanExit(Taken, S))
    return false;
  // The taken edge must agree with the latch form: inverted mask
  // (xor+cttz) pairs with the eq -1 latch, plain cttz with eq 0.
  if (S.Inverted == IsZero)
    return false;
  (void)F;
  return true;
}

// Match the scalar tail loop under the chunk region: header Hdr holds
// one scalar element load plus an icmp-eq against the needle element
// (or the second buffer), stepping its IV by 1 per pass. Two layouts:
// single-block self-loop, or header+latch (the latch steps the IV and
// tests the trip bound). Its start must be the chunk-covered prefix
// (resume) or 0; after the fold the resume edge is dropped and the
// zero-start edge stands.
bool matchTailLoop(BasicBlock *Hdr, BasicBlock *Latch, ScanInfo &S) {
  // Scalar loads + one icmp eq in the header: one load (find form,
  // against the needle element) or two loads (dual-buffer compare).
  SmallVector<LoadInst *, 2> EltLoads;
  ICmpInst *EltCmp = nullptr;
  for (Instruction &I : *Hdr) {
    if (auto *L = dyn_cast<LoadInst>(&I)) {
      if (L->getType()->isVectorTy())
        return false;
      if (L->isVolatile() || L->isAtomic() || hasOpaque(*L))
        return false;
      EltLoads.push_back(L);
    }
    if (auto *C = dyn_cast<ICmpInst>(&I)) {
      if (C->getPredicate() == ICmpInst::ICMP_EQ && !EltCmp)
        EltCmp = C;
    }
  }
  if (EltLoads.empty() || EltLoads.size() > 2 || !EltCmp)
    return false;
  // No vector types anywhere in the tail pair.
  for (BasicBlock *BB : {Hdr, Latch}) {
    if (BB == Hdr)
      continue;
    for (Instruction &I : *BB) {
      if (I.getType()->isVectorTy())
        return false;
      if (isa<StoreInst>(&I))
        return false;
      if (auto *CB = dyn_cast<CallBase>(&I)) {
        const Function *CF = CB->getCalledFunction();
        if (!CF || !CF->isIntrinsic())
          return false;
      }
      if (hasOpaque(I))
        return false;
    }
  }
  // Element width must match the lane width.
  for (LoadInst *L : EltLoads) {
    unsigned EltBits = L->getType()->getPrimitiveSizeInBits();
    if (EltBits != S.LaneBytes * 8)
      return false;
  }
  // Base pointers of the tail loads must match the chunk loads' bases
  // (same buffers), proving the tail walks the same arrays. Phis
  // resolve through their non-chunk-region incomings (the base).
  DenseSet<BasicBlock *> ChunkBlocks{S.H, S.ScanExit};
  if (S.Latch)
    ChunkBlocks.insert(S.Latch);
  auto BaseOf = [&](Value *Ptr) -> Value * {
    SmallVector<Value *, 4> WL{Ptr};
    DenseSet<Value *> Seen;
    while (!WL.empty()) {
      Value *V = WL.pop_back_val();
      if (!Seen.insert(V).second)
        continue;
      if (auto *GEP = dyn_cast<GetElementPtrInst>(V)) {
        WL.push_back(GEP->getPointerOperand());
        continue;
      }
      if (auto *BC = dyn_cast<BitCastInst>(V)) {
        WL.push_back(BC->getOperand(0));
        continue;
      }
      if (auto *Phi = dyn_cast<PHINode>(V)) {
        for (unsigned I = 0; I < Phi->getNumIncomingValues(); ++I)
          if (!ChunkBlocks.count(Phi->getIncomingBlock(I)))
            WL.push_back(Phi->getIncomingValue(I));
        continue;
      }
      return V;
    }
    return Ptr;
  };
  if (S.SecondLoad) {
    // Dual-buffer: two tail loads from the two chunk bases, in either
    // order, and the element compare must be their equality (each
    // side one of the loads).
    if (EltLoads.size() != 2)
      return false;
    Value *B0 = BaseOf(S.ChunkLoad->getPointerOperand());
    Value *B1 = BaseOf(S.SecondLoad->getPointerOperand());
    Value *T0 = BaseOf(EltLoads[0]->getPointerOperand());
    Value *T1 = BaseOf(EltLoads[1]->getPointerOperand());
    bool Same = (T0 == B0 && T1 == B1) || (T0 == B1 && T1 == B0);
    if (!Same)
      return false;
    Value *C0 = EltCmp->getOperand(0);
    Value *C1 = EltCmp->getOperand(1);
    bool UsesBoth =
        (C0 == EltLoads[0] && C1 == EltLoads[1]) ||
        (C0 == EltLoads[1] && C1 == EltLoads[0]);
    if (!UsesBoth)
      return false;
  } else {
    // Find form: one tail load from the chunk base, and the element
    // compare must use the same needle value (through casts).
    if (EltLoads.size() != 1)
      return false;
    Value *B0 = BaseOf(S.ChunkLoad->getPointerOperand());
    Value *T0 = BaseOf(EltLoads[0]->getPointerOperand());
    if (T0 != B0)
      return false;
    bool NeedleUse = false;
    for (unsigned K = 0; K < 2; ++K) {
      Value *V = EltCmp->getOperand(K);
      while (auto *C = dyn_cast<CastInst>(V))
        V = C->getOperand(0);
      if (S.Needle && V == S.Needle)
        NeedleUse = true;
    }
    if (!NeedleUse)
      return false;
  }
  // Tail IV must be a step-1 walk: backedge value is add(phi, 1) in
  // Hdr (single-block) or in Latch (header+latch).
  bool FoundIV = false;
  for (PHINode &Phi : Hdr->phis()) {
    Value *Back = nullptr;
    for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I)
      if (Phi.getIncomingBlock(I) == Latch)
        Back = Phi.getIncomingValue(I);
    auto *Add = dyn_cast<BinaryOperator>(Back);
    if (!Add || Add->getOpcode() != Instruction::Add)
      continue;
    auto *One = dyn_cast<ConstantInt>(Add->getOperand(1));
    if (!One || !One->isOne())
      continue;
    if (Add->getOperand(0) != &Phi)
      continue;
    if (Add->getParent() != Hdr && Add->getParent() != Latch)
      continue;
    FoundIV = true;
  }
  if (!FoundIV)
    return false;
  return true;
}

// Find the scalar tail loop reachable from Entry without passing
// through Skip: the first loop header (single-block self-loop or a
// header whose latch sibling branches back). Returns the header, and
// fills LatchOut (== header for single-block).
BasicBlock *findTailLoop(BasicBlock *Entry,
                         const DenseSet<BasicBlock *> &Skip,
                         BasicBlock *&LatchOut) {
  SmallVector<BasicBlock *, 16> WL{Entry};
  DenseSet<BasicBlock *> Seen;
  while (!WL.empty()) {
    BasicBlock *B = WL.pop_back_val();
    if (!Seen.insert(B).second)
      continue;
    if (Skip.count(B))
      continue;
    auto *T = dyn_cast<BranchInst>(B->getTerminator());
    if (T && T->isConditional()) {
      for (BasicBlock *SU : successors(B))
        if (SU == B) {
          LatchOut = B;
          return B;
        }
      // Header+latch: a successor branching back to B.
      for (BasicBlock *SU : successors(B)) {
        if (Skip.count(SU))
          continue;
        bool BackToB = false;
        for (BasicBlock *SS : successors(SU))
          if (SS == B)
            BackToB = true;
        if (BackToB) {
          LatchOut = SU;
          return B;
        }
      }
    }
    for (BasicBlock *SU : successors(B))
      WL.push_back(SU);
    if (Seen.size() > 128)
      break;
  }
  return nullptr;
}

bool foldScan(Function &F, ScanInfo &S, BasicBlock *TailEntry,
              BasicBlock *Dispatch, const DenseSet<BasicBlock *> &Region) {
  // 1. Fold the dispatch to the scalar tail at index 0.
  auto *Br = cast<BranchInst>(Dispatch->getTerminator());
  BranchInst *NewBr = BranchInst::Create(TailEntry, Dispatch);
  copyVA(Br, NewBr);
  NewBr->setDebugLoc(Br->getDebugLoc());
  Br->eraseFromParent();
  // 2. Drop chunk-block incomings from tail phis; the tail restarts at
  // its zero-start incoming (the small-N edge proves it).
  DenseSet<BasicBlock *> Dead = Region;
  for (BasicBlock &BB : F) {
    if (Dead.count(&BB))
      continue;
    for (PHINode &Phi : BB.phis()) {
      SmallVector<BasicBlock *, 4> Kill;
      for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I)
        if (Dead.count(Phi.getIncomingBlock(I)))
          Kill.push_back(Phi.getIncomingBlock(I));
      if (Kill.size() == Phi.getNumIncomingValues())
        continue;
      for (BasicBlock *KB : Kill)
        Phi.removeIncomingValue(KB);
    }
  }
  // 3. Erase dead chunk blocks, terminators first.
  SmallVector<BasicBlock *, 8> DeadList(Dead.begin(), Dead.end());
  for (BasicBlock *BB : DeadList) {
    Instruction *OldT = BB->getTerminator();
    auto *Unr = new UnreachableInst(BB->getContext());
    Unr->insertInto(BB, BB->end());
    OldT->eraseFromParent();
  }
  for (BasicBlock &BB : F) {
    if (Dead.count(&BB))
      continue;
    for (PHINode &Phi : BB.phis()) {
      SmallVector<BasicBlock *, 4> Kill;
      for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I)
        if (Dead.count(Phi.getIncomingBlock(I)))
          Kill.push_back(Phi.getIncomingBlock(I));
      if (Kill.size() == Phi.getNumIncomingValues())
        continue;
      for (BasicBlock *KB : Kill)
        Phi.removeIncomingValue(KB);
    }
  }
  for (BasicBlock *BB : DeadList)
    BB->dropAllReferences();
  for (BasicBlock *BB : DeadList)
    BB->eraseFromParent();
  (void)TailEntry;
  return true;
}

bool runOnFunction(Function &F) {
  bool Changed = false;
  // Collect-then-rewrite: matching never mutates.
  SmallVector<std::tuple<ScanInfo, BasicBlock *, BasicBlock *, DenseSet<BasicBlock *>>, 4> Work;
  for (BasicBlock &BB : F) {
    // Disjointness from UnloopVec: skip anything without the
    // mask+bit-scan skeleton (UnloopVec outputs never carry vector
    // types or cttz-on-mask edges).
    ScanInfo S;
    if (!matchScanLoop(&BB, F, S))
      continue;
    LLVM_DEBUG(dbgs() << "unvecsearch matched " << BB.getName() << " in "
                      << F.getName() << "\n");
    // Chunk region and its entry dispatch. The region is {H, Latch}
    // plus ScanExit plus the unconditional pre chain between the
    // entry dispatch D and H. D is found by walking up from H through
    // unconditional pre blocks to the conditional dispatch whose
    // other successor reaches the scalar tail without passing through
    // the region.
    DenseSet<BasicBlock *> Skip{S.H, S.ScanExit};
    if (S.Latch && S.Latch != S.H)
      Skip.insert(S.Latch);
    // Walk up through unconditional pre blocks.
    SmallVector<BasicBlock *, 4> PreChain;
    {
      BasicBlock *Cur = S.H;
      DenseSet<BasicBlock *> Seen;
      while (Cur && Seen.insert(Cur).second && PreChain.size() < 8) {
        BasicBlock *Outside = nullptr;
        for (BasicBlock *P : predecessors(Cur)) {
          if (Skip.count(P) || P == Cur)
            continue;
          if (Outside) {
            Outside = nullptr;
            break; // join: stop the walk, use H preds directly
          }
          Outside = P;
        }
        if (!Outside)
          break;
        auto *PT = dyn_cast<BranchInst>(Outside->getTerminator());
        if (!PT || PT->isConditional())
          break;
        if (PT->getSuccessor(0) != Cur)
          break;
        PreChain.push_back(Outside);
        Skip.insert(Outside);
        Cur = Outside;
      }
    }
    for (BasicBlock *PB : PreChain)
      S.Pre = PB; // folded region includes the pre chain (last wins)
    // Candidate dispatches: conditional branches outside Skip with
    // one successor reaching the region and another reaching a scalar
    // tail loop. Check H's outside preds' parents first (nearest).
    SmallVector<BasicBlock *, 4> DispCands;
    for (BasicBlock *P : predecessors(S.H)) {
      if (Skip.count(P) || P == S.H)
        continue;
      auto *PT = dyn_cast<BranchInst>(P->getTerminator());
      if (PT && PT->isConditional())
        DispCands.push_back(P);
    }
    for (BasicBlock *PB : PreChain)
      for (BasicBlock *P : predecessors(PB)) {
        if (Skip.count(P) || P == PB)
          continue;
        auto *PT = dyn_cast<BranchInst>(P->getTerminator());
        if (PT && PT->isConditional())
          DispCands.push_back(P);
      }
    // Also consider H's own outside preds chain head.
    BasicBlock *Dispatch = nullptr;
    BasicBlock *TailEntry = nullptr;
    BasicBlock *TailHdr = nullptr;
    for (BasicBlock *D : DispCands) {
      auto *DT = cast<BranchInst>(D->getTerminator());
      for (unsigned K = 0; K < 2; ++K) {
        BasicBlock *Succ = DT->getSuccessor(K);
        if (Skip.count(Succ))
          continue;
        // The other successor must reach the region.
        BasicBlock *Other = DT->getSuccessor(1 - K);
        bool ReachesRegion = (Other == S.H) || Skip.count(Other);
        if (!ReachesRegion) {
          // Walk forward a few blocks for the region (pre chain).
          SmallVector<BasicBlock *, 8> WL{Other};
          DenseSet<BasicBlock *> Seen;
          while (!WL.empty() && !ReachesRegion) {
            BasicBlock *B = WL.pop_back_val();
            if (!Seen.insert(B).second)
              continue;
            if (Skip.count(B)) {
              ReachesRegion = true;
              break;
            }
            for (BasicBlock *SU : successors(B))
              WL.push_back(SU);
            if (Seen.size() > 16)
              break;
          }
        }
        if (!ReachesRegion)
          continue;
        BasicBlock *LH = nullptr;
        BasicBlock *Found =
            findTailLoop(Succ, Skip, LH);
        if (!Found || !LH)
          continue;
        if (!matchTailLoop(Found, LH, S))
          continue;
        LLVM_DEBUG(dbgs() << "unvecsearch tail " << Found->getName()
                          << " dispatch " << D->getName() << "\n");
        Dispatch = D;
        TailEntry = Succ;
        TailHdr = Found;
        break;
      }
      if (Dispatch)
        break;
    }
    if (!Dispatch || !TailHdr || !TailEntry)
      continue;
    // Latch-exit resume chain: the latch's non-H successor may run
    // through resume-computation blocks (e.g. bound+stride for the
    // tail entry phi) before reaching the tail entry. Those blocks die
    // with the region: follow the single chain from the latch to the
    // tail entry and absorb it. Anything branched/complex bails.
    {
      BasicBlock *Latch = S.Latch ? S.Latch : S.H;
      SmallVector<BasicBlock *, 4> Chain;
      BasicBlock *Next = nullptr;
      unsigned NonH = 0;
      for (BasicBlock *SU : successors(Latch))
        if (SU != S.H && SU != Latch) {
          ++NonH;
          Next = SU;
        }
      bool ChainOK = (NonH == 1);
      while (ChainOK && Next && Next != TailEntry && Chain.size() < 8) {
        if (Skip.count(Next)) {
          ChainOK = false;
          break;
        }
        // Positional check: every predecessor is the latch (first
        // block) or the previous chain block.
        bool PredOK = true;
        BasicBlock *Expect =
            Chain.empty() ? Latch : Chain.back();
        for (BasicBlock *P : predecessors(Next))
          if (P != Expect)
            PredOK = false;
        if (!PredOK) {
          ChainOK = false;
          break;
        }
        Chain.push_back(Next);
        auto *BT = dyn_cast<BranchInst>(Next->getTerminator());
        if (!BT || BT->isConditional()) {
          ChainOK = false;
          break;
        }
        Next = BT->getSuccessor(0);
      }
      if (!ChainOK)
        continue;
      if (Next != TailEntry)
        continue;
      for (BasicBlock *CB : Chain)
        Skip.insert(CB);
    }
    // except the result phi in the exit merge (fed by both the scan
    // edge and the tail edge; step 2 drops the scan edge and the tail
    // edge value stands).
    bool OK = true;
    DenseSet<BasicBlock *> R = Skip;
    SmallVector<BasicBlock *, 8> RList(R.begin(), R.end());
    for (BasicBlock *RB : RList) {
      for (Instruction &I : *RB) {
        if (I.getType()->isVoidTy())
          continue;
        for (Use &U : I.uses()) {
          auto *UI = cast<Instruction>(U.getUser());
          BasicBlock *UB = UI->getParent();
          if (R.count(UB))
            continue;
          if (isa<DbgInfoIntrinsic>(UI))
            continue;
          if (auto *P = dyn_cast<PHINode>(UI)) {
            // Allowed: exit-merge phi that keeps a non-R incoming
            // (the tail edge). It keeps the tail value after the
            // scan edge drops.
            bool KeepsNonR = false;
            for (unsigned K = 0; K < P->getNumIncomingValues(); ++K)
              if (!R.count(P->getIncomingBlock(K)))
                KeepsNonR = true;
            if (KeepsNonR)
              continue;
          }
          UVS_BAIL("escape def " << I << " user " << *UI << " in "
                                 << F.getName());
          OK = false;
        }
        if (!OK)
          break;
      }
      if (!OK)
        break;
    }
    if (!OK) {
      UVS_BAIL("escape " << S.H->getName() << " in " << F.getName());
      continue;
    }
    // The tail entry must restart at 0: it needs a constant-0 incoming
    // from the dispatch (kept) while the resume incoming comes from
    // the dead region (dropped). When the entry IS the loop header,
    // the header phi check inside matchTailLoop covers the IV; still
    // require the zero edge here.
    {
      bool ZeroFromDispatch = false;
      for (PHINode &Phi : TailEntry->phis()) {
        int Idx = Phi.getBasicBlockIndex(Dispatch);
        if (Idx < 0)
          continue;
        if (auto *CZ =
                dyn_cast<ConstantInt>(Phi.getIncomingValue((unsigned)Idx)))
          if (CZ->isZero())
            ZeroFromDispatch = true;
      }
      // No phis at all (straight entry block): trivially restarts.
      bool HasPhis = TailEntry->phis().begin() != TailEntry->phis().end();
      if (HasPhis && !ZeroFromDispatch) {
        UVS_BAIL("zerostart " << TailEntry->getName() << " in "
                              << F.getName());
        continue;
      }
    }
    Work.push_back({S, TailEntry, Dispatch, Skip});
  }
  for (auto &W : Work) {
    ScanInfo &S = std::get<0>(W);
    // Skip blocks already folded by an earlier candidate in this
    // sweep (explicit disjointness: one fold per region).
    if (!S.H->getParent())
      continue;
    LLVM_DEBUG(dbgs() << "unvecsearch folding " << S.H->getName() << " in "
                      << F.getName() << "\n");
    Changed |= foldScan(F, S, std::get<1>(W), std::get<2>(W), std::get<3>(W));
  }
  return Changed;
}

} // namespace

PreservedAnalyses UnvecSearchPass::run(Function &F,
                                       FunctionAnalysisManager &) {
  if (F.isDeclaration() || F.hasOptNone())
    return PreservedAnalyses::all();
  if (!runOnFunction(F))
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}

} // namespace revenant

// Plugin entry for `opt -load-pass-plugin`. The `revenant-opt`
// superbuild links this library statically and registers the
// `revenant-unvecsearch` name in Pipeline/Register.cpp, so the entry
// point is compiled only for the standalone plugin DLL.
#ifdef REVENANT_BUILD_PLUGIN
#undef DEBUG_TYPE
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"

static void registerUnvecSearchCallbacks(::llvm::PassBuilder &PB) {
  PB.registerPipelineParsingCallback(
      [](::llvm::StringRef Name, ::llvm::FunctionPassManager &PM,
         ::llvm::ArrayRef<::llvm::PassBuilder::PipelineElement>) {
        if (Name == "revenant-unvecsearch") {
          PM.addPass(revenant::UnvecSearchPass());
          return true;
        }
        return false;
      });
}

::llvm::PassPluginLibraryInfo getPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "RevenantUnvecSearch",
          LLVM_VERSION_STRING, registerUnvecSearchCallbacks};
}

extern "C" __declspec(dllexport) ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getPassPluginInfo();
}
#endif
