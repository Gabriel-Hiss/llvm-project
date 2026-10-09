//===- Lane0.cpp - lane-0 scalar web detection ---------------------------===//
//
// Part of the Revenant LLVM fork. See Lane0.h.
//
// LLVM-IR mapping of the reference (`lane0.rs`, which works on the
// plain-Rust IR with `__vecinsert`/`__vecextract` intrinsics and `i128`
// pseudo-vectors):
// - `i128` value -> fixed-width LLVM vector type (`<N x T>`; the lifter's
//   xmm-only model arrives here as `<4 x i32>` / `<2 x i64>` / `<4 x
//   float>` / `<2 x double>` and friends).
// - `__vecinsert{eb}(x, s, 0)` -> `insertelement <N x T> x, T s, i32 0`.
// - `__vecextract{eb}(x, 0)` -> `extractelement <N x T> x, i32 0`.
// - `trunc i128 -> <=64` -> `trunc` / `bitcast` / `fptrunc` narrowing to
//   at most 64 bits.
// - `bitcast i128 -> i128` -> vector `bitcast` between vector types.
// - `zext/sext small -> i128` -> scalar `zext`/`sext` producing a vector
//   type (kept only when the web element type agrees).
//
//===----------------------------------------------------------------------===//

#include "Lane0.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/InstrTypes.h"

using namespace llvm;

namespace revenant {
namespace lane0 {

static bool isZeroIndex(const Value *Idx) {
  if (auto *C = dyn_cast<ConstantInt>(Idx))
    return C->isZero();
  return false;
}

static bool isVectorTy(const Value *V) {
  return isa<FixedVectorType>(V->getType());
}

Role roleOf(const Instruction *User, const Value *V) {
  // extractelement v, 0 -> Scalar.
  if (auto *EI = dyn_cast<ExtractElementInst>(User)) {
    if (EI->getVectorOperand() == V && isZeroIndex(EI->getIndexOperand()))
      return Role::Scalar;
    return Role::Poison;
  }
  // insertelement with v as base, scalar second operand, lane 0.
  if (auto *II = dyn_cast<InsertElementInst>(User)) {
    if (II->getOperand(0) == V && !isVectorTy(II->getOperand(1)) &&
        isZeroIndex(II->getOperand(2)))
      return Role::Transparent;
    return Role::Poison;
  }
  // Narrowing cast to <= 64 bits reads lane 0 (reference: trunc -> <=64).
  if (auto *CI = dyn_cast<CastInst>(User)) {
    if (CI->getOperand(0) != V)
      return Role::Poison;
    unsigned ToBits = CI->getDestTy()->getScalarSizeInBits();
    switch (CI->getOpcode()) {
    case Instruction::Trunc:
      return ToBits <= 64 ? Role::Scalar : Role::Poison;
    case Instruction::BitCast:
      if (isa<FixedVectorType>(CI->getDestTy()))
        return Role::Transparent;
      // Equal-width scalar bitcast of the lane (e.g. i64 <-> double).
      return ToBits <= 64 ? Role::Scalar : Role::Poison;
    default:
      return Role::Poison;
    }
  }
  // phi whose incoming list references V forwards lane-ness (reference:
  // every phi is transparent; shrink drops the web when the dst is not a
  // member). A phi merely *named* in some other position (impossible in
  // valid IR, but cheap to check) must not count as a forward edge.
  if (auto *Phi = dyn_cast<PHINode>(User)) {
    for (Value *Inc : Phi->incoming_values())
      if (Inc == V)
        return Role::Transparent;
    return Role::Poison;
  }
  return Role::Poison;
}

bool isRepresentable(const Instruction *Def) {
  if (isa<PHINode>(Def) || isa<LoadInst>(Def))
    return true;
  if (auto *CI = dyn_cast<CastInst>(Def)) {
    if (CI->getOpcode() == Instruction::BitCast &&
        isa<FixedVectorType>(CI->getDestTy()))
      return true;
    if ((CI->getOpcode() == Instruction::ZExt ||
         CI->getOpcode() == Instruction::SExt) &&
        CI->getSrcTy()->isIntOrIntVectorTy())
      return true;
    return false;
  }
  return insertParts(Def).has_value();
}

std::optional<std::pair<unsigned, Value *>>
insertParts(const Instruction *I) {
  auto *II = dyn_cast<InsertElementInst>(I);
  if (!II || !isVectorTy(II->getOperand(0)) || isVectorTy(II->getOperand(1)) ||
      !isZeroIndex(II->getOperand(2)))
    return std::nullopt;
  unsigned EB = II->getOperand(1)->getType()->getScalarSizeInBits();
  if (EB == 0 || EB > 64)
    return std::nullopt;
  return std::make_pair(EB, II->getOperand(1));
}

bool inputsScalarized(const Instruction *Def,
                      const DenseSet<const Value *> &Lane0) {
  if (auto *Phi = dyn_cast<PHINode>(Def)) {
    for (Value *Inc : Phi->incoming_values()) {
      if (isa<Constant>(Inc) && !isa<GlobalValue>(Inc))
        continue;
      if (!Lane0.contains(Inc))
        return false;
    }
    return true;
  }
  if (auto *CI = dyn_cast<CastInst>(Def)) {
    if (CI->getOpcode() == Instruction::BitCast &&
        isa<FixedVectorType>(CI->getDestTy()))
      return Lane0.contains(CI->getOperand(0));
  }
  return true;
}

void shrink(Function &F,
            const DenseMap<const Value *, const Instruction *> &Defs128,
            const DenseMap<const Value *, SmallVector<const Instruction *, 4>>
                &Users,
            DenseSet<const Value *> &Lane0) {
  (void)F;
  bool Progress = true;
  while (Progress) {
    Progress = false;
    SmallVector<const Value *, 8> Dropped;
    for (const Value *V : Lane0) {
      bool BadUse = false;
      auto UI = Users.find(V);
      if (UI != Users.end()) {
        for (const Instruction *User : UI->second) {
          switch (roleOf(User, V)) {
          case Role::Scalar:
            break;
          case Role::Transparent: {
            // The forward target is the user instruction's own value
            // (phi, bitcast, insertelement all define their dst). It
            // must itself be a member; note User is `const`.
            const Value *Dst =
                dyn_cast<Value>(const_cast<Instruction *>(User));
            if (!Dst || !Lane0.contains(Dst))
              BadUse = true;
            break;
          }
          case Role::Poison:
            BadUse = true;
            break;
          }
          if (BadUse)
            break;
        }
      }
      // Terminator uses keep full width (reference: returned values and
      // branch operands poison).
      if (!BadUse) {
        for (const User *U : V->users()) {
          if (isa<ReturnInst>(U) || isa<BranchInst>(U) || isa<SwitchInst>(U) ||
              isa<InvokeInst>(U) || isa<ResumeInst>(U)) {
            BadUse = true;
            break;
          }
        }
      }
      auto DI = Defs128.find(V);
      if (!BadUse && DI != Defs128.end() &&
          !inputsScalarized(DI->second, Lane0))
        BadUse = true;
      if (BadUse)
        Dropped.push_back(V);
    }
    for (const Value *V : Dropped) {
      Lane0.erase(V);
      Progress = true;
    }
  }
}

SmallVector<SmallVector<const Value *, 8>, 4>
webs(const DenseMap<const Value *, SmallVector<const Instruction *, 4>> &Users,
     const DenseSet<const Value *> &Lane0) {
  DenseMap<const Value *, SmallVector<const Value *, 4>> Adj;
  for (const Value *V : Lane0) {
    auto UI = Users.find(V);
    if (UI == Users.end())
      continue;
    for (const Instruction *User : UI->second) {
      if (roleOf(User, V) != Role::Transparent)
        continue;
      const Value *Dst =
          dyn_cast<Value>(const_cast<Instruction *>(User));
      if (Dst && Lane0.contains(Dst)) {
        Adj[V].push_back(Dst);
        Adj[Dst].push_back(V);
      }
    }
  }
  SmallVector<const Value *, 16> Order(Lane0.begin(), Lane0.end());
  sort(Order, [](const Value *A, const Value *B) {
    return A->getName() < B->getName();
  });
  DenseSet<const Value *> Seen;
  SmallVector<SmallVector<const Value *, 8>, 4> Out;
  SmallVector<const Value *, 8> Stack;
  for (const Value *V : Order) {
    if (!Seen.insert(V).second)
      continue;
    SmallVector<const Value *, 8> Web;
    Web.push_back(V);
    Stack.push_back(V);
    while (!Stack.empty()) {
      const Value *X = Stack.pop_back_val();
      auto AI = Adj.find(X);
      if (AI == Adj.end())
        continue;
      for (const Value *Y : AI->second) {
        if (Seen.insert(Y).second) {
          Web.push_back(Y);
          Stack.push_back(Y);
        }
      }
    }
    Out.push_back(std::move(Web));
  }
  return Out;
}

// Agree helper: one slot, one value across the web.
template <typename T> static bool agree(std::optional<T> &Slot, T X) {
  if (Slot)
    return *Slot == X;
  Slot = X;
  return true;
}

static bool isScalarElemTy(Type *T) {
  return T->isIntegerTy(8) || T->isIntegerTy(16) || T->isIntegerTy(32) ||
         T->isIntegerTy(64) || T->isFloatTy() || T->isDoubleTy();
}

Type *webElemTy(Function &F,
                const DenseMap<const Value *, const Instruction *> &Defs128,
                const DenseMap<const Value *, SmallVector<const Instruction *, 4>>
                    &Users,
                const SmallVector<const Value *, 8> &Web) {
  std::optional<unsigned> EB;
  std::optional<bool> Float;
  for (const Value *V : Web) {
    auto DI = Defs128.find(V);
    if (DI == Defs128.end())
      return nullptr;
    if (auto Ins = insertParts(DI->second)) {
      if (!agree(EB, Ins->first))
        return nullptr;
      Type *ST = Ins->second->getType();
      if (isa<Constant>(Ins->second)) {
        // Packed-constant insert: float-ness follows the web; width must
        // still agree. Constants constrain width only.
      } else {
        if (!isScalarElemTy(ST) ||
            ST->getScalarSizeInBits() != Ins->first ||
            !agree(Float, ST->isFloatingPointTy()))
          return nullptr;
      }
      if (ST->isFloatingPointTy() && !agree(Float, true))
        return nullptr;
    }
    auto UI = Users.find(V);
    if (UI == Users.end())
      continue;
    for (const Instruction *User : UI->second) {
      if (auto *EI = dyn_cast<ExtractElementInst>(User)) {
        if (EI->getVectorOperand() != V || !isZeroIndex(EI->getIndexOperand()))
          continue;
        Type *RT = EI->getType();
        if (!isScalarElemTy(RT))
          return nullptr;
        if (!agree(EB, RT->getScalarSizeInBits()))
          return nullptr;
        if (!agree(Float, RT->isFloatingPointTy()))
          return nullptr;
      } else if (auto *CI = dyn_cast<CastInst>(User)) {
        if (CI->getOperand(0) != V || CI->getOpcode() != Instruction::Trunc)
          continue;
        unsigned ToBits = CI->getDestTy()->getScalarSizeInBits();
        if (Float && *Float) {
          if (ToBits != *EB)
            return nullptr;
        } else if (EB && ToBits > *EB) {
          return nullptr;
        }
      }
    }
  }
  unsigned Width = EB ? *EB : 64;
  bool IsFloat = Float ? *Float : false;
  if (Width > 64 || (IsFloat && Width != 32 && Width != 64))
    return nullptr;
  LLVMContext &Ctx = F.getContext();
  if (IsFloat)
    return Width == 32 ? Type::getFloatTy(Ctx) : Type::getDoubleTy(Ctx);
  return Type::getIntNTy(Ctx, Width);
}

} // namespace lane0
} // namespace revenant
