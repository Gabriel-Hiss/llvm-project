//===- UncxxUtil.h - shared matchers for inline STL recovery ----*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// Byte-offset and field-access matchers shared by the Uncxx rewriter
// groups. Lifted IR and optimized compiler IR both address STL fields as
// `base + const` (either `getelementptr i8, base, K` or a struct-typed GEP
// whose constant byte offset is K), so every matcher here works in raw
// byte offsets and never trusts type names.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_UNCXX_UTIL_H
#define REVENANT_TRANSFORMS_UNCXX_UTIL_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Support/Casting.h"

#include "revenant/Transforms/Uncxx/Uncxx.h"

#include <cstdint>
#include <optional>

namespace revenant {
namespace uncxx {

using namespace llvm;

//--- address math --------------------------------------------------------//

/// Constant byte offset of a GEP, or nullopt when not constant.
inline std::optional<int64_t> gepByteOffset(const llvm::DataLayout &DL,
                                            const llvm::GEPOperator *G) {
  llvm::APInt Off(DL.getIndexTypeSizeInBits(G->getPointerOperandType()), 0);
  if (!G->accumulateConstantOffset(DL, Off))
    return std::nullopt;
  return Off.getSExtValue();
}

/// Strip zero-offset GEPs and bitcasts; returns the base object.
inline llvm::Value *baseOf(llvm::Value *V) {
  while (true) {
    if (auto *BC = llvm::dyn_cast<llvm::BitCastOperator>(V)) {
      V = BC->getOperand(0);
      continue;
    }
    if (auto *G = llvm::dyn_cast<llvm::GEPOperator>(V)) {
      bool Zero = true;
      for (auto &Ix : G->indices())
        if (auto *C = llvm::dyn_cast<llvm::ConstantInt>(Ix)) {
          if (!C->isZero())
            Zero = false;
        } else
          Zero = false;
      if (Zero) {
        V = G->getPointerOperand();
        continue;
      }
    }
    return V;
  }
}

/// Split `V` into the root object below all constant GEP/bitcast layers
/// plus the total byte offset. Fails (returns nullptr) on any
/// non-constant layer: phis, selects, pointer arithmetic.
inline llvm::Value *stripToRoot(llvm::Value *V, const llvm::DataLayout &DL,
                                int64_t &Total) {
  Total = 0;
  while (true) {
    if (auto *BC = llvm::dyn_cast<llvm::BitCastOperator>(V)) {
      V = BC->getOperand(0);
      continue;
    }
    if (auto *G = llvm::dyn_cast<llvm::GEPOperator>(V)) {
      auto B = gepByteOffset(DL, G);
      if (!B)
        return nullptr;
      Total += *B;
      V = G->getPointerOperand();
      continue;
    }
    return V;
  }
}

/// The address computation that reads field `Off` bytes into `Base`
/// (`Base` itself for Off==0, or a GEP `Base+Off`). Collects loads through
/// it: lifted IR may compute the GEP once per access or share it.
inline void loadsAtField(const llvm::DataLayout &DL, llvm::Value *Base,
                         int64_t Off, llvm::SmallVectorImpl<llvm::LoadInst *> &Out) {
  for (llvm::User *U : Base->users()) {
    if (Off == 0) {
      if (auto *L = llvm::dyn_cast<llvm::LoadInst>(U))
        if (L->getPointerOperand() == Base && L->isUnordered())
          Out.push_back(L);
    }
    auto *G = llvm::dyn_cast<llvm::GEPOperator>(U);
    if (!G)
      continue;
    if (baseOf(G->getPointerOperand()) != Base)
      continue;
    auto B = gepByteOffset(DL, G);
    if (!B || *B != Off)
      continue;
    auto *I = llvm::dyn_cast<llvm::Instruction>(U);
    if (!I)
      continue;
    for (llvm::User *UU : I->users())
      if (auto *L = llvm::dyn_cast<llvm::LoadInst>(UU))
        if (L->getPointerOperand() == I && L->isUnordered())
          Out.push_back(L);
  }
}

/// First load through `Base + Off` (convenience wrapper).
inline llvm::LoadInst *firstLoadAt(const llvm::DataLayout &DL,
                                   llvm::Value *Base, int64_t Off) {
  llvm::SmallVector<llvm::LoadInst *, 4> Ls;
  loadsAtField(DL, Base, Off, Ls);
  return Ls.empty() ? nullptr : Ls.front();
}

/// The pointer value `P` addresses `Base+Off`: `P == Base` (Off 0) or a
/// GEP with constant byte offset Off from `baseOf(P) == Base`.
inline bool isFieldAddr(const llvm::DataLayout &DL, llvm::Value *P,
                        llvm::Value *Base, int64_t Off) {
  if (Off == 0 && P == Base)
    return true;
  auto *G = llvm::dyn_cast<llvm::GEPOperator>(P);
  if (!G || baseOf(G->getPointerOperand()) != Base)
    return false;
  auto B = gepByteOffset(DL, G);
  return B && *B == Off;
}

/// If `L` is `load Base+Off` (byte offset), return Off.
inline std::optional<int64_t> loadFieldOff(const llvm::DataLayout &DL,
                                           const llvm::LoadInst *L,
                                           llvm::Value *Base) {
  llvm::Value *P = const_cast<llvm::Value*>(L->getPointerOperand());
  if (P == Base)
    return int64_t(0);
  auto *G = llvm::dyn_cast<llvm::GEPOperator>(P);
  if (!G || baseOf(G->getPointerOperand()) != Base)
    return std::nullopt;
  return gepByteOffset(DL, G);
}

/// `V` is `ptrtoint (load Base+Off)` or the load itself.
inline std::optional<int64_t> loadedFieldOff(const llvm::DataLayout &DL,
                                             llvm::Value *V,
                                             llvm::Value *Base) {
  if (auto *C = llvm::dyn_cast<llvm::CastInst>(V))
    V = C->getOperand(0);
  auto *L = llvm::dyn_cast<llvm::LoadInst>(V);
  if (!L)
    return std::nullopt;
  return loadFieldOff(DL, L, Base);
}

/// Pointer difference `sub`/`ptrtoint` pair: returns the two pointer loads
/// when `V` is `((int)X) - ((int)Y)` with X/Y loads of Base's fields.
inline bool matchFieldDiff(const llvm::DataLayout &DL, llvm::Value *V,
                           llvm::Value *Base, int64_t &OffA, int64_t &OffB) {
  using namespace llvm::PatternMatch;
  llvm::Value *A = nullptr, *B = nullptr;
  if (!match(V, m_Sub(m_Value(A), m_Value(B))))
    return false;
  auto RA = loadedFieldOff(DL, A, Base);
  auto RB = loadedFieldOff(DL, B, Base);
  if (!RA || !RB)
    return false;
  OffA = *RA;
  OffB = *RB;
  return true;
}

/// Divide-by-element-size around a difference: `sdiv X, C`, `udiv X, C`,
/// `ashr X, log2C`, or `lshr X, log2C`. Returns the element size (in
/// bytes) when X is the diff and C a positive constant: a power of two
/// for shifts, any positive value up to 4KiB for divisions.
///
/// Exactness is deliberately not required. The pointers feeding the diff
/// are a container's begin/end (or the equivalent field pair), which the
/// container invariant keeps element-aligned: the remainder is always
/// zero, so `lshr`/`udiv` without `exact` compute the same value an
/// exact division would. The emitted helper is specified as the exact
/// element count, which matches on every reachable execution.
inline std::optional<int64_t> matchElemSizeDiv(llvm::Instruction *I,
                                               llvm::Value *&Diff) {
  using namespace llvm::PatternMatch;
  const llvm::APInt *C;
  if (match(I, m_SDiv(m_Value(Diff), m_APInt(C))) ||
      match(I, m_UDiv(m_Value(Diff), m_APInt(C)))) {
    if (C->isStrictlyPositive() && C->ule(4096))
      return (int64_t)C->getZExtValue();
    return std::nullopt;
  }
  if (match(I, m_AShr(m_Value(Diff), m_APInt(C))) ||
      match(I, m_LShr(m_Value(Diff), m_APInt(C)))) {
    if (C->ult(63))
      return int64_t(1) << C->getZExtValue();
  }
  return std::nullopt;
}

/// Get or declare `revenant.cxx.<Name>` and emit a call before `Before`,
/// copying provenance (`!revenant.va`, debug loc) from `Prov`.
llvm::CallInst *emitCxxCall(llvm::Module &M, llvm::StringRef Name,
                            llvm::Type *RetTy,
                            llvm::ArrayRef<llvm::Value *> Args,
                            llvm::Instruction *Before,
                            const llvm::Instruction *Prov);

/// Get or declare the helper (no call emitted). An existing declaration
/// with a clashing signature gets a numeric suffix; helpers are emitted
/// per call-site signature.
llvm::Function *cxxHelper(llvm::Module &M, llvm::StringRef Name,
                          llvm::Type *RetTy,
                          llvm::ArrayRef<llvm::Type *> ArgTys);

/// Copy `!revenant.va` and the debug location onto `Dst` from `Src`.
/// Only the provenance attachment is copied: alias/range/nonnull markers
/// describe the old instruction, not the call.
void copyProv(llvm::Instruction &Dst, const llvm::Instruction &Src);

/// Erase an instruction left without users. Only called on instructions
/// the matcher consumed (pure address math or loads the emitted helper
/// re-reads, so no fault behavior is lost).
void eraseIfDead(llvm::Instruction *I);

/// True when `I` carries `!revenant.opaque`: lifted asm with no IR
/// semantics. Matchers never consume these.
inline bool hasOpaque(const llvm::Instruction &I) {
  return I.getMetadata("revenant.opaque") != nullptr;
}

/// STL flavor of the module under analysis, resolved from the flag
/// (`-revenant-uncxx-stl` / `REVENANT_UNCXX_STL`) or symbol evidence.
/// Unknown means no STL evidence: only flavor-independent shapes run.
CxxFlavor flavorOf(const llvm::Module &M);

const char *flavorName(CxxFlavor Fl);

/// True when `CB` calls a `revenant.cxx.*` helper this pass emitted (used
/// to keep fixed-point iterations from matching their own output).
bool isCxxHelperCall(const llvm::CallBase &CB);

/// True when `CB` is a call to an operator-delete-like callee
/// (`??3@`, `_ZdlPv`, `free`, `RtlFreeHeap`...), i.e. a plain heap free.
bool isDeleteCall(const llvm::CallBase &CB);

/// True when `CB` deallocates: operator delete (see `isDeleteCall`) or a
/// sized/unsized delete-shaped helper the STL calls (`_Deallocate...`,
/// `deallocate@...`, `operator delete` Itanium spellings).
bool isDeallocCall(const llvm::CallBase &CB);

/// True when `BB` is a canary arm: `llvm.trap` (or a noreturn throw
/// helper) followed by `unreachable`. Used to accept the MSVC
/// manually-vector-aligned free diamond inside tidy sequences.
bool isTrapBlock(const llvm::BasicBlock &BB);

//--- purity ------------------------------------------------------------//

/// True when every instruction in `[First, Last]` (same block, inclusive)
/// is free of memory writes, throwing calls, and opaque markers.
/// Reads, pure arithmetic, and `llvm.*` markers are fine.
bool rangeIsPure(llvm::BasicBlock::iterator First,
                 llvm::BasicBlock::iterator Last);

/// True when every user of `V` sits in `BB` after `From` and the span up
/// to the last user is pure (see `rangeIsPure`). Guarantees a rewritten
/// snapshot (loads folded into a call at the end of the span) cannot
/// observe an intervening write.
bool allUsersInPureRange(llvm::Value *V, llvm::BasicBlock *BB,
                         llvm::BasicBlock::iterator From);

/// True when `Def` dominates `Use`: every path from the entry to `Use`
/// crosses `Def` (same-block order included). Local iterative check,
/// no analysis manager needed (the plugin path cannot query analyses
/// stock opt did not register).
bool dominates(const llvm::Instruction &Def, const llvm::Instruction &Use);

//--- rewriter groups (Uncxx.cpp / UncxxBlocks.cpp) ---------------------//

// Single-block accessor folds (vector size/data/index, string
// size/data, empty checks). Implemented in Uncxx.cpp.
unsigned runScalarIdioms(llvm::Function &F, const llvm::DataLayout &DL,
                         CxxFlavor Fl);


/// Single-block accessor folds (vector size/data/index, string
/// size/data, empty checks). Returns the number of rewrites applied.
/// (Implemented in Uncxx.cpp.)
unsigned runScalarIdioms(llvm::Function &F, const llvm::DataLayout &DL,
                         CxxFlavor Fl);

/// Multi-block idioms: dtor/tidy diamonds, push_back/append fast+grow
/// diamonds, guarded deletes, incref/decref diamonds, tree-step node
/// hops. Returns the number of rewrites applied.
unsigned runBlockIdioms(llvm::Function &F, const llvm::DataLayout &DL,
                         CxxFlavor Fl, void *DTReserved = nullptr);

} // namespace uncxx
} // namespace revenant

#endif // REVENANT_TRANSFORMS_UNCXX_UTIL_H
