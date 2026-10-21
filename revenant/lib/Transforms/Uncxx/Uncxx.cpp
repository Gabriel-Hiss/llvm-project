//===- Uncxx.cpp - recover inlined C++ standard-library calls --*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// At -O1+ the optimizer pastes small STL helpers into callers: vector
// size/data/operator[]/push_back/pop_back fast paths, basic_string SSO
// decode/size/append fast paths, vector/string _Tidy dispose sequences,
// shared_ptr incref/decref diamonds, unique_ptr guarded dispose bodies,
// plain new/delete pair shapes. The lifted IR then shows raw field loads
// and pointer bumps where the source had a member call.
//
// This pass recognizes those shapes and rebuilds the member-shaped call
// as `revenant.cxx.<name>`, keeping exact semantics. The helper set:
//
//   vector.size / vector.data / vector.front / vector.back /
//   vector.operator[] / vector.at / vector.push_back / vector.pop_back /
//   vector.clear / vector.empty / vector.capacity / vector.begin_ptr /
//   vector.end_ptr / vector_dtor / vector_tidy_known_empty /
//   string.size / string.data / string.c_str / string.empty /
//   string.operator[] / string.at / string_dtor / string_tidy_known_empty /
//   string_append_space / string_push_back / string_clear_guarded /
//   shared_incref / shared_decref_maybe_dispose /
//   unique_dispose_guarded / unique_reset_guarded /
//   new_array / delete_guarded / delete_array_guarded /
//   iterator_loop / sized_dealloc
//
// Complements revenant-patterns (owner: MuseTxPatterns): patterns matches
// contiguous single-block regions against a compiled oracle catalog;
// this pass is table-free, works in byte offsets (never type names),
// and owns the multi-block idioms (capacity guard + growth-call
// diamonds, refcount diamonds, dtor/tidy diamonds, guarded deletes).
// Patterns keeps everything it already rewrites. The boundary: anything
// needing a compiled snippet oracle or a whole-function canonical shape
// stays with patterns; anything below (byte-offset field idioms with
// inline layout knowledge) lives here.
//
// Flavor gating: MSVC STL, libstdc++, and libc++ lay vector/string
// fields out differently (MSVC string keeps size at +16/capacity at +24
// with a 16-byte inline buffer; libstdc++ string keeps {ptr,len}
// plus a capacity-or-local union; libc++ packs short size into the
// last byte). The `-revenant-uncxx-stl` flag (or `REVENANT_UNCXX_STL`
// for the plugin path; `auto` by default) picks the layout, or infers
// it from module symbols (MSVC `?...@std@@` / `__CxxFrameHandler3`,
// Itanium `_Z...`, libc++ `std::__1` / `_LIBCPP_`). Unknown means no
// STL evidence: only flavor-independent shapes run (guarded deletes,
// sized deallocs, refcount atomics, new/delete pairs).
//
// Firing rule: only airtight matches rewrite. Every memory access in a
// consumed region is accounted for by the matched idiom, no opaque
// (`revenant.asm.*`) call is ever consumed, and `!revenant.region`
// terminators are never touched. Every emitted call copies `!revenant.va`
// and the debug location from the instruction it replaces.
//
// Alive2 sketches for the non-obvious folds live above each matcher.
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/Uncxx/Uncxx.h"
#include "UncxxUtil.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#ifdef REVENANT_BUILD_PLUGIN
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#endif

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string>

using namespace llvm;
using namespace llvm::PatternMatch;

#define DEBUG_TYPE "revenant-uncxx"

namespace revenant {
namespace uncxx {
namespace {

//--- STL field layouts ---------------------------------------------------//
//
// All offsets are bytes from the container object base. MSVC and
// libstdc++/libc++ agree on vector ({first,last,end} = +0/+8/+16) but
// disagree on string; the string matchers below take every layout they
// run under from this table. See the MSVC `_String_val` (`_Bx` union at
// +0, `_Mysize` at +16, `_Myres` at +24), libstdc++ `basic_string`
// (`_M_p` at +0, `_M_string_length` at +8, capacity-or-local union at
// +16), and libc++ `__basic_string_common` (data at +0, size at +8,
// short-mask byte inside the last word).

constexpr int64_t VecFirst = 0, VecLast = 8, VecEnd = 16;

struct StringLayout {
  int64_t Data;   // heap pointer field (long mode)
  int64_t Size;   // length field
  int64_t Cap;    // capacity field (long mode)
  int64_t SsoCap; // inline capacity (short mode)
};

constexpr StringLayout MsvcString{0, 16, 24, 15};
constexpr StringLayout GnuString{0, 8, 16, 15};
constexpr StringLayout LibcxxString{0, 8, 16, 22};

StringLayout stringLayout(CxxFlavor Fl) {
  switch (Fl) {
  case CxxFlavor::Gnu:
    return GnuString;
  case CxxFlavor::Libcxx:
    return LibcxxString;
  case CxxFlavor::Msvc:
  case CxxFlavor::Auto:
    return MsvcString;
  }
  return MsvcString;
}

constexpr int64_t RefUses = 8;  // _Ref_count_base::_Uses (both MSVC/libc++)
constexpr int64_t RefWeak = 12; // _Ref_count_base::_Weaks
constexpr int64_t UpPtr = 0;    // unique_ptr::_M_ptr / _Mypair held pointer

//--- small helpers -------------------------------------------------------//

/// True when `V` is a null pointer constant (any pointer type).
bool isNullPtr(Value *V) {
  if (isa<ConstantPointerNull>(V))
    return true;
  if (auto *C = dyn_cast<ConstantInt>(V))
    return C->isZero();
  return false;
}

bool matchConstantZero(Value *V);

/// Strip pointer casts and zero-offset GEPs.
Value *stripPtr(Value *V) {
  while (true) {
    if (auto *BC = dyn_cast<BitCastOperator>(V)) {
      V = BC->getOperand(0);
      continue;
    }
    if (auto *G = dyn_cast<GEPOperator>(V)) {
      bool Zero = true;
      for (auto &Ix : G->indices())
        if (auto *C = dyn_cast<ConstantInt>(Ix)) {
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

/// Root object and byte offset of a pointer value, or nullopt on any
/// non-constant layer. Flavor-independent: strips through arbitrarily
/// nested constant GEP chains (`_Compressed_pair`/`_Vector_val` member
/// addressing in -O0, flat `i8` GEPs in -O2).
std::optional<std::pair<Value *, int64_t>> rootOff(const DataLayout &DL,
                                                   Value *V) {
  int64_t Total = 0;
  Value *R = stripToRoot(V, DL, Total);
  if (!R)
    return std::nullopt;
  return std::make_pair(R, Total);
}

/// Load of `Base (+ Off)`: through a GEP, or directly (Off == 0). Gives
/// up on ambiguous shapes (more than one candidate load) so a rewrite
/// never merges two live values. Skips loads feeding stores of the same
/// address (partially inlined helpers recompute fields after storing).
LoadInst *singleLoadAt(const DataLayout &DL, Value *Base, int64_t Off) {
  SmallVector<LoadInst *, 4> Ls;
  loadsAtField(DL, Base, Off, Ls);
  if (Off == 0) {
    // Direct loads of the base: keep only unordered ones.
    SmallVector<LoadInst *, 4> Kept;
    for (LoadInst *L : Ls)
      if (L->isUnordered())
        Kept.push_back(L);
    Ls = std::move(Kept);
  }
  if (Ls.size() != 1)
    return nullptr;
  return Ls.front();
}

/// Non-null `Base` dominates the values it feeds: true when `Base` is an
/// argument, a global, or defined outside loops. Used as a cheap guard
/// against matching temporaries the optimizer materialized mid-block.

/// Accept layout-based recovery only when the same object is also passed to
/// a function matched by the libdb as an STL member.
bool hasLibraryMemberEvidence(const DataLayout &DL, Value *Base,
                              Function &F) {
  for (BasicBlock &BB : F)
    for (Instruction &I : BB) {
      auto *CB = dyn_cast<CallBase>(&I);
      if (!CB)
        continue;
      Value *Called = CB->getCalledOperand()->stripPointerCastsAndAliases();
      const Function *Callee = dyn_cast<Function>(Called);
      if (!Callee)
        continue;
      Attribute A = Callee->getFnAttribute("revenant.lib");
      if (!A.isStringAttribute())
        continue;
      StringRef Name = A.getValueAsString();
      bool IsSTLMember =
          (Name.contains("vector") || Name.contains("basic_string")) &&
          (Name.contains("data") || Name.contains("size") ||
           Name.contains("push_back") || Name.contains("operator[]") ||
           Name.contains("_Emplace") || Name.contains("_Change_array") ||
           Name.contains("begin") || Name.contains("end"));
      if (!IsSTLMember)
        continue;
      for (Value *Arg : CB->args()) {
        if (stripPtr(Arg) == Base)
          return true;
        auto Root = rootOff(DL, Arg);
        if (Root && Root->first == Base)
          return true;
      }
    }
  return false;
}

/// Container kind from the offset set used against `Base` anywhere in
/// the function: vector touches {0,8,16}, MSVC string touches
/// {0-or-buf,16,24}. Returns 1 for vector, 2 for string, 0 when
/// ambiguous. Loads that were already folded (replaced by helper
/// calls) are gone, so this runs on the current IR each time: kinds
/// only become MORE ambiguous as folds proceed, never less, which is
/// the safe direction (ambiguous => no fold).
int containerKind(const DataLayout &DL, Value *Base, Function &F) {
  if (!hasLibraryMemberEvidence(DL, Base, F))
    return 0;
  bool H0 = false, H8 = false, H16 = false, H24 = false;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      Value *P = nullptr;
      if (auto *L = dyn_cast<LoadInst>(&I))
        P = L->getPointerOperand();
      else if (auto *S = dyn_cast<StoreInst>(&I))
        P = S->getPointerOperand();
      else if (auto *G = dyn_cast<GEPOperator>(&I))
        P = G;
      else
        continue;
      int64_t T = 0;
      Value *R = stripToRoot(const_cast<Value *>(P), DL, T);
      if (R != Base)
        continue;
      switch (T) {
      case 0: H0 = true; break;
      case 8: H8 = true; break;
      case 16: H16 = true; break;
      case 24: H24 = true; break;
      default: break;
      }
    }
  }
  // Calls already folded read the fields inside the helper: count our
  // own helper calls as evidence too.
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      auto *CB = dyn_cast<CallBase>(&I);
      if (!CB)
        continue;
      const Function *CF = CB->getCalledFunction();
      if (!CF)
        continue;
      StringRef N = CF->getName();
      if (!N.consume_front("revenant.cxx."))
        continue;
      if (CB->arg_empty())
        continue;
      if (stripPtr(CB->getArgOperand(0)) != Base)
        continue;
      if (N.starts_with("vector."))
        return 1;
      if (N.starts_with("string."))
        return 2;
    }
  }
  // Offset +16 is both MSVC string size and vector end. Treat it as a
  // string field only when the object also has the MSVC capacity field
  // and does not have the vector middle pointer at +8.
  if (H24 && H16 && H0 && !H8)
    return 2;
  // GNU and libc++ strings have size at +8; vector has a pointer at +0
  // and +16, so require that distinctive pair before calling it a vector.
  if (H8 && H16 && H0)
    return 1;
  return 0;
}

bool stableBase(Value *Base, Function &F) {
  if (isa<Argument>(Base) || isa<GlobalValue>(Base))
    return true;
  if (auto *I = dyn_cast<Instruction>(Base))
    return I->getFunction() == &F && !I->getParent()->getName().empty();
  return false;
}

//--- vector/string scalar matchers ---------------------------------------//
//
// Each matcher below folds one inlined accessor back to a helper call.
// Shapes were read off cpp-clang-o2 main.post.ll (vector_ops/string_ops
// at O2) and the matching main.pre.ll member calls; the pre/post pair
// is the spec. Example: pre calls `size()`; post shows
// `sub ((int)load end) ((int)load begin)` plus an exact `sdiv`/`lshr`
// by the element size. The fold is exact on every execution because the
// container invariant keeps both pointers element-aligned.

// size(): (end - begin) / elem, via div or shift. Covers vector::size
// (pre: `?size@?$vector...` call; post: diff + sdiv/lshr) and the
// pointerless `end - begin` spelling the loop vectorizer leaves.
bool matchVectorSize(const DataLayout &DL, Instruction *I, Value *&Vec,
                     int64_t &Elem) {
  Value *Diff = nullptr;
  auto ES = matchElemSizeDiv(I, Diff);
  if (!ES || *ES <= 0 || *ES > 4096)
    return false;
  // The diff must be (load end) - (load begin): walk ptrtoint/casts.
  Value *A = nullptr, *B = nullptr;
  if (!match(Diff, m_Sub(m_Value(A), m_Value(B))))
    return false;
  auto stripInt = [](Value *V) -> Value * {
    if (auto *C = dyn_cast<CastInst>(V))
      return C->getOperand(0);
    return V;
  };
  auto *LA = dyn_cast<LoadInst>(stripInt(A));
  auto *LB = dyn_cast<LoadInst>(stripInt(B));
  if (!LA || !LB)
    return false;
  auto RA = rootOff(DL, LA->getPointerOperand());
  auto RB = rootOff(DL, LB->getPointerOperand());
  if (!RA || !RB || RA->first != RB->first)
    return false;
  // MSVC/libstdc++/libc++ vector: {first,last,end}; size = last-first.
  // Accept both (first,last) orders; the subtraction direction picks
  // which field is which.
  bool Fwd = (RA->second == VecLast && RB->second == VecFirst);
  bool Rev = (RA->second == VecFirst && RB->second == VecLast);
  if (!Fwd && !Rev)
    return false;
  Vec = RA->first;
  Elem = *ES;
  return true;
}

// data()/begin_ptr(): load of first, or the SSO select (data field vs
// inline buffer) for strings. Vector side: `load Base+0` with no other
// vector field touched between the load and the use.
bool matchVectorData(const DataLayout &DL, LoadInst *L, Value *&Vec) {
  // The first field is a pointer: non-pointer loads at +0 belong to
  // some other struct sharing the offset.
  if (!L->getType()->isPointerTy())
    return false;
  auto R = rootOff(DL, L->getPointerOperand());
  if (!R || R->second != VecFirst)
    return false;
  Vec = R->first;
  return true;
}

// operator[]/at/front/back: `load (gep (load first) + idx*elem)`.
// The address must be a GEP off the loaded first pointer with a
// variable index; constant-index GEPs are front/back.
bool matchVectorIndex(const DataLayout &DL, LoadInst *L, Value *&Vec,
                      Value *&Idx, int64_t &Elem) {
  Value *P = L->getPointerOperand();
  auto *G = dyn_cast<GEPOperator>(P);
  if (!G)
    return false;
  // Base of the GEP must be (a cast of) a load of first.
  Value *B = stripPtr(G->getPointerOperand());
  auto *BL = dyn_cast<LoadInst>(B);
  if (!BL)
    return false;
  auto R = rootOff(DL, BL->getPointerOperand());
  if (!R || R->second != VecFirst)
    return false;
  // Element size from the GEP's own type or the index scale.
  int64_t Scale = DL.getTypeStoreSize(L->getType());
  if (Scale <= 0 || Scale > 1024)
    return false;
  Vec = R->first;
  Idx = nullptr;
  Elem = Scale;
  // Index operand: the non-zero GEP index (struct GEPs have been
  // flattened to byte offsets by rootOff; the remaining GEP is the
  // element index).
  for (auto &Ix : G->indices()) {
    if (auto *C = dyn_cast<ConstantInt>(Ix)) {
      if (C->isZero())
        continue;
      // Constant byte offset: front/back shape, no index value.
      return true;
    }
    Idx = Ix;
    return true;
  }
  return true;
}
// back(): load from `(load Base+last) - 1`. This differs from front/index:
// the element address is based on the vector's last pointer, not its first.
bool matchVectorBack(const DataLayout &DL, LoadInst *L, Value *&Vec) {
  auto *G = dyn_cast<GEPOperator>(L->getPointerOperand());
  if (!G)
    return false;
  auto *Last = dyn_cast<LoadInst>(stripPtr(G->getPointerOperand()));
  if (!Last || !Last->getType()->isPointerTy())
    return false;
  auto R = rootOff(DL, Last->getPointerOperand());
  if (!R || R->second != VecLast)
    return false;
  bool SawMinusOne = false;
  for (Value *Ix : G->indices()) {
    auto *C = dyn_cast<ConstantInt>(Ix);
    if (!C)
      return false;
    if (C->isMinusOne())
      SawMinusOne = true;
    else if (!C->isZero())
      return false;
  }
  if (!SawMinusOne)
    return false;
  Vec = R->first;
  return true;
}

bool hasVectorExtent(const DataLayout &DL, Value *Base, Function &F) {
  if (containerKind(DL, Base, F) == 1)
    return true;
  SmallVector<LoadInst *, 4> Firsts, Lasts;
  loadsAtField(DL, Base, VecFirst, Firsts);
  loadsAtField(DL, Base, VecLast, Lasts);
  Firsts.erase(std::remove_if(Firsts.begin(), Firsts.end(),
                              [](LoadInst *L) {
                                return !L->getType()->isPointerTy();
                              }),
               Firsts.end());
  Lasts.erase(std::remove_if(Lasts.begin(), Lasts.end(),
                             [](LoadInst *L) {
                               return !L->getType()->isPointerTy();
                             }),
              Lasts.end());
  if (Firsts.empty() || Lasts.empty())
    return false;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (auto *Cmp = dyn_cast<ICmpInst>(&I)) {
        if (Cmp->getPredicate() != ICmpInst::ICMP_EQ &&
            Cmp->getPredicate() != ICmpInst::ICMP_NE)
          continue;
        Value *A = stripPtr(Cmp->getOperand(0));
        Value *B = stripPtr(Cmp->getOperand(1));
        for (LoadInst *First : Firsts)
          for (LoadInst *Last : Lasts)
            if ((A == First && B == Last) || (A == Last && B == First))
              return true;
      }
  return false;
}

bool matchVectorBackAddress(const DataLayout &DL, GetElementPtrInst *G,
                            Value *&Vec, Function &F) {
  auto *Last = dyn_cast<LoadInst>(stripPtr(G->getPointerOperand()));
  if (!Last || !Last->getType()->isPointerTy())
    return false;
  auto R = rootOff(DL, Last->getPointerOperand());
  if (!R || R->second != VecLast || !hasVectorExtent(DL, R->first, F))
    return false;
  auto Offset = gepByteOffset(DL, cast<GEPOperator>(G));
  if (!Offset || *Offset >= 0 || *Offset < -4096)
    return false;
  Vec = R->first;
  return true;
}

// string.size(): load of the size field (MSVC +16, gnu/libc++ +8).
// The load must be the only string-field access feeding the value and
// post-dominate any SSO branch (callers check allUsersInPureRange).
bool matchStringSize(const DataLayout &DL, LoadInst *L, CxxFlavor Fl,
                     Value *&Str) {
  if (!L->getType()->isIntegerTy())
    return false;
  StringLayout SL = stringLayout(Fl);
  auto R = rootOff(DL, L->getPointerOperand());
  if (!R || R->second != SL.Size)
    return false;
  Str = R->first;
  return true;
}

// string.data()/c_str(): the SSO select `select (cap > SsoCap) data
// short-base`, or a bare data-field load (long mode known live).
bool matchStringData(const DataLayout &DL, Value *V, CxxFlavor Fl,
                     Value *&Str) {
  StringLayout SL = stringLayout(Fl);
  if (auto *SI = dyn_cast<SelectInst>(V)) {
    // Condition must compare the capacity field against SsoCap.
    Value *C = SI->getCondition();
    Value *A = SI->getTrueValue(), *B = SI->getFalseValue();
    auto isDataLoad = [&](Value *X) -> Value * {
      if (auto *L = dyn_cast<LoadInst>(stripPtr(X))) {
        if (!L->getType()->isPointerTy())
          return (Value *)nullptr;
        auto R = rootOff(DL, L->getPointerOperand());
        if (R && R->second == SL.Data)
          return R->first;
      }
      return nullptr;
    };
    // Either arm may be the heap pointer (select order varies).
    if (Value *S = isDataLoad(A))
      Str = S;
    else if (Value *S = isDataLoad(B))
      Str = S;
    else
      return false;
    // An SSO selector must compare this string's capacity against the
    // inline threshold and select the heap pointer exactly when long mode
    // is active. A null check on data with an arbitrary fallback is not
    // evidence of a string accessor.
    auto *Cmp = dyn_cast<ICmpInst>(C);
    if (!Cmp)
      return false;
    bool HeapWhenTrue = false;
    bool HasModeCompare = false;
    for (unsigned I = 0; I != 2; ++I) {
      Value *CapV = Cmp->getOperand(I);
      Value *LimitV = Cmp->getOperand(1 - I);
      auto *CapLoad = dyn_cast<LoadInst>(stripPtr(CapV));
      auto *Limit = dyn_cast<ConstantInt>(LimitV);
      if (!CapLoad || !Limit)
        continue;
      auto R = rootOff(DL, CapLoad->getPointerOperand());
      if (!R || R->first != Str || R->second != SL.Cap)
        continue;
      ICmpInst::Predicate Pred = I == 0 ? Cmp->getPredicate()
                                       : Cmp->getSwappedPredicate();
      bool IsInlineLimit = Limit->equalsInt(SL.SsoCap);
      bool IsNextLimit = Limit->equalsInt(SL.SsoCap + 1);
      if (IsInlineLimit && Pred == ICmpInst::ICMP_UGT) {
        HeapWhenTrue = true;
        HasModeCompare = true;
      } else if (IsInlineLimit && Pred == ICmpInst::ICMP_ULE) {
        HeapWhenTrue = false;
        HasModeCompare = true;
      } else if (IsNextLimit && Pred == ICmpInst::ICMP_UGE) {
        HeapWhenTrue = true;
        HasModeCompare = true;
      } else if (IsNextLimit && Pred == ICmpInst::ICMP_ULT) {
        HeapWhenTrue = false;
        HasModeCompare = true;
      }
    }
    bool DataWhenTrue = isDataLoad(A) == Str;
    if (!HasModeCompare || DataWhenTrue != HeapWhenTrue)
      return false;
    Value *Inline = DataWhenTrue ? B : A;
    if (Inline != Str) {
      auto R = rootOff(DL, Inline);
      if (!R || R->first != Str || R->second != 0)
        return false;
    }
    return true;
  }
  if (auto *L = dyn_cast<LoadInst>(V)) {
    if (!L->getType()->isPointerTy())
      return false;
    auto R = rootOff(DL, L->getPointerOperand());
    if (!R || R->second != SL.Data)
      return false;
    Str = R->first;
    return true;
  }
  return false;
}

bool matchConstantZero(Value *V) {
  if (auto *C = dyn_cast<ConstantInt>(V))
    return C->isZero();
  return isNullPtr(V);
}

// empty(): size load compared against zero, or begin==end / first==last.
bool matchEmpty(const DataLayout &DL, ICmpInst *Cmp, CxxFlavor Fl,
                Value *&Obj, bool &IsString) {
  if (Cmp->getPredicate() != ICmpInst::ICMP_EQ &&
      Cmp->getPredicate() != ICmpInst::ICMP_NE)
    return false;
  Value *A = Cmp->getOperand(0), *B = Cmp->getOperand(1);
  // size == 0 spelling.
  for (auto [X, Y] : {std::make_pair(A, B), std::make_pair(B, A)}) {
    if (auto *L = dyn_cast<LoadInst>(stripPtr(X))) {
      if (!matchConstantZero(Y))
        continue;
      StringLayout SL = stringLayout(Fl);
      auto R = rootOff(DL, L->getPointerOperand());
      if (R && R->second == SL.Size) {
        Obj = R->first;
        IsString = true;
        return true;
      }
    }
  }
  // Both operands must be this object's exact first/last field loads.
  // A loop-exit comparison such as `it != end` is not vector.empty().
  auto *LA = dyn_cast<LoadInst>(stripPtr(A));
  auto *LB = dyn_cast<LoadInst>(stripPtr(B));
  if (!LA || !LB || !LA->getType()->isPointerTy() ||
      !LB->getType()->isPointerTy())
    return false;
  auto RA = rootOff(DL, LA->getPointerOperand());
  auto RB = rootOff(DL, LB->getPointerOperand());
  if (!RA || !RB || RA->first != RB->first)
    return false;
  bool FirstLast =
      (RA->second == VecFirst && RB->second == VecLast) ||
      (RA->second == VecLast && RB->second == VecFirst);
  if (!FirstLast)
    return false;
  Obj = RA->first;
  IsString = false;
  return true;
}

bool isVectorEmptyOperand(const DataLayout &DL, LoadInst *L) {
  auto R = rootOff(DL, L->getPointerOperand());
  if (!R || (R->second != VecFirst && R->second != VecLast))
    return false;
  int64_t OtherOff = R->second == VecFirst ? VecLast : VecFirst;
  for (User *U : L->users()) {
    auto *Cmp = dyn_cast<ICmpInst>(U);
    if (!Cmp || (Cmp->getPredicate() != ICmpInst::ICMP_EQ &&
                 Cmp->getPredicate() != ICmpInst::ICMP_NE))
      continue;
    Value *Other = Cmp->getOperand(0) == L ? Cmp->getOperand(1)
                                           : Cmp->getOperand(0);
    auto *OtherLoad = dyn_cast<LoadInst>(stripPtr(Other));
    if (!OtherLoad)
      continue;
    auto OtherRoot = rootOff(DL, OtherLoad->getPointerOperand());
    if (OtherRoot && OtherRoot->first == R->first &&
        OtherRoot->second == OtherOff)
      return true;
  }
  return false;
}

// Match `icmp (pred) (load Base+Field) <const>`; returns the base on
// success. Used for capacity-guard and SSO-capacity comparisons.
LoadInst *matchFieldCmp(const DataLayout &DL, ICmpInst &Cmp, Value *Base,
                        int64_t Field) {
  for (Value *Op : {Cmp.getOperand(0), Cmp.getOperand(1)}) {
    if (auto *L = dyn_cast<LoadInst>(stripPtr(Op))) {
      auto R = rootOff(DL, L->getPointerOperand());
      if (R && R->first == Base && R->second == Field)
        return L;
    }
  }
  return nullptr;
}

//--- scalar rewrite driver -----------------------------------------------//
//
// Walks each block once, folding inlined accessors bottom-up. One
// instruction per iteration restarts the scan: folds change def-use
// chains (a size div feeds a compare), and the corpus shapes are small
// enough that quadratic scan cost never matters.

bool isUncxxHelperName(StringRef N) {
  return N.consume_front("revenant.cxx.");
}

} // namespace

unsigned runScalarIdioms(Function &F, const DataLayout &DL, CxxFlavor Fl) {
  unsigned N = 0;
  Module *M = F.getParent();
  StringLayout SL = stringLayout(Fl);
  bool KnownFlavor = Fl != CxxFlavor::Auto;

  for (unsigned Iter = 0; Iter < 4; ++Iter) {
    bool Any = false;
    for (BasicBlock &BB : F) {
      for (auto It = BB.begin(); It != BB.end();) {
        Instruction &I = *It++;
        if (hasOpaque(I) || I.isTerminator() || isa<PHINode>(&I) ||
            isa<DbgInfoIntrinsic>(&I))
          continue;
        if (auto *CB = dyn_cast<CallBase>(&I)) {
          if (CB->isInlineAsm())
            continue;
          // Never match our own output on re-runs.
          if (const Function *CF = CB->getCalledFunction())
            if (isUncxxHelperName(CF->getName()))
              continue;
          // vector::at / string::at bounds call stays; only the
          // element load folds (handled at the load below).
          continue;
        }
        Value *Obj = nullptr;
        int64_t Elem = 0;
        // vector.size: div/shift of the field diff.
        if (!I.getType()->isVoidTy() && matchVectorSize(DL, &I, Obj, Elem)) {
          if (!stableBase(Obj, F) || !allUsersInPureRange(&I, &BB, It))
            continue;
          Type *I64 = Type::getInt64Ty(M->getContext());
          auto *C = emitCxxCall(*M, "vector.size", I64, {Obj}, &I, &I);
          Value *R = C;
          if (I.getType() != I64 && I.getType()->isIntegerTy()) {
            auto *T = CastInst::CreateIntegerCast(
                C, I.getType(), /*isSigned=*/false, "", &I);
            copyProv(*T, I);
            R = T;
          } else if (I.getType() != I64) {
            continue; // non-integer size spelling: leave it
          }
          I.replaceAllUsesWith(R);
          eraseIfDead(&I);
          ++N;
          Any = true;
          continue;
        }
        // vector.data / begin_ptr: bare first load.
        if (auto *G = dyn_cast<GetElementPtrInst>(&I)) {
          if (matchVectorBackAddress(DL, G, Obj, F) &&
              KnownFlavor && stableBase(Obj, F)) {
            auto *C = emitCxxCall(*M, "vector.back", G->getType(),
                                  {Obj}, G, G);
            G->replaceAllUsesWith(C);
            eraseIfDead(G);
            ++N;
            Any = true;
            continue;
          }
        }
        if (auto *L = dyn_cast<LoadInst>(&I)) {
          if (L->isUnordered() && matchVectorData(DL, L, Obj) &&
              !isVectorEmptyOperand(DL, L)) {
            // String data loads share offset +0 on MSVC: disambiguate
            // by requiring the vector end field to exist nearby
            // (a load of Base+16 in the same function) when the
            // flavor is known; unknown flavor skips data folds.
            if (!KnownFlavor)
              continue;
            if (!stableBase(Obj, F))
              continue;
            if (containerKind(DL, Obj, F) != 1)
              continue; // probably a string heap pointer, not vector
            Type *PT = L->getType();
            auto *C =
                emitCxxCall(*M, "vector.data", PT, {Obj}, L, L);
            L->replaceAllUsesWith(C);
            eraseIfDead(L);
            ++N;
            Any = true;
            continue;
          }
          if (L->isUnordered() && matchVectorBack(DL, L, Obj)) {
            if (!KnownFlavor || !stableBase(Obj, F) ||
                containerKind(DL, Obj, F) != 1)
              continue;
            auto *C = emitCxxCall(*M, "vector.back", L->getType(),
                                  {Obj}, L, L);
            L->replaceAllUsesWith(C);
            eraseIfDead(L);
            ++N;
            Any = true;
            continue;
          }
          // vector.operator[]/at: element load off loaded first.
          Value *Idx = nullptr;
          if (L->isUnordered() &&
              matchVectorIndex(DL, L, Obj, Idx, Elem)) {
            if (!KnownFlavor || !stableBase(Obj, F))
              continue;
            if (containerKind(DL, Obj, F) != 1)
              continue; // probably a string heap pointer, not vector
            Type *ET = L->getType();
            char NB[64];
            snprintf(NB, sizeof NB, "vector.index.i%u",
                     (unsigned)ET->getPrimitiveSizeInBits());
            Type *IdxT = Idx ? Idx->getType()
                             : Type::getInt64Ty(M->getContext());
            Value *IdxV = Idx ? Idx
                              : ConstantInt::get(IdxT, 0);
            auto *C = emitCxxCall(*M, NB, ET, {Obj, IdxV}, L, L);
            L->replaceAllUsesWith(C);
            eraseIfDead(L);
            ++N;
            Any = true;
            continue;
          }
          // string.size: size-field load.
          if (L->isUnordered() && KnownFlavor &&
              matchStringSize(DL, L, Fl, Obj)) {
            // Disambiguate from vector end-field (+16, MSVC size
            // shares the offset): only fold when the base walks like
            // a string (touches size+capacity, not last/end).
            if (containerKind(DL, Obj, F) != 2)
              continue; // vector end, not string size
            if (!stableBase(Obj, F))
              continue;
            Type *I64 = Type::getInt64Ty(M->getContext());
            auto *C =
                emitCxxCall(*M, "string.size", I64, {Obj}, L, L);
            Value *R = C;
            if (L->getType() != I64 && L->getType()->isIntegerTy()) {
              auto *T = CastInst::CreateIntegerCast(
                  C, L->getType(), /*isSigned=*/false, "", L);
              copyProv(*T, *L);
              R = T;
            } else if (L->getType() != I64) {
              continue; // non-integer size spelling: leave it
            }
            L->replaceAllUsesWith(R);
            eraseIfDead(L);
            ++N;
            Any = true;
            continue;
          }
          // string.data/c_str: data-field load or SSO select handled
          // at the select below; bare loads fold here.
          if (L->isUnordered() && KnownFlavor) {
            auto R = rootOff(DL, L->getPointerOperand());
            if (R && R->second == SL.Data) {
              // Same vector/string ambiguity as above.
              if (containerKind(DL, R->first, F) == 2 &&
                  stableBase(R->first, F)) {
                Type *PT = L->getType();
                auto *C = emitCxxCall(*M, "string.data", PT,
                                      {R->first}, L, L);
                L->replaceAllUsesWith(C);
                eraseIfDead(L);
                ++N;
                Any = true;
                continue;
              }
            }
          }
        }
        // string.data SSO select.
        if (auto *SI = dyn_cast<SelectInst>(&I)) {
          if (KnownFlavor && matchStringData(DL, SI, Fl, Obj)) {
            if (!stableBase(Obj, F))
              continue;
            auto *C = emitCxxCall(*M, "string.data", SI->getType(),
                                  {Obj}, SI, SI);
            SI->replaceAllUsesWith(C);
            eraseIfDead(SI);
            ++N;
            Any = true;
            continue;
          }
        }
        // empty(): size/first-last compare against zero/null.
        if (auto *Cmp = dyn_cast<ICmpInst>(&I)) {
          Value *O = nullptr;
          bool IsStr = false;
          if (matchEmpty(DL, Cmp, Fl, O, IsStr)) {
            if (!stableBase(O, F))
              continue;
            if (IsStr && !KnownFlavor)
              continue;
            // Same vector/string ambiguity as the size fold: only
            // fire when the base walks like the claimed container.
            if (containerKind(DL, O, F) != (IsStr ? 2 : 1))
              continue;
            auto *C = emitCxxCall(
                *M, IsStr ? "string.empty" : "vector.empty",
                Type::getInt1Ty(M->getContext()), {O}, Cmp, Cmp);
            Cmp->replaceAllUsesWith(C);
            eraseIfDead(Cmp);
            ++N;
            Any = true;
            continue;
          }
          // capacity(): end-field load used by a compare stays a
          // load; the compare itself folds when it is end-vs-last.
          // (Handled by the load folds above; nothing to do here.)
        }
      }
    }
    if (!Any)
      break;
  }
  return N;
}

} // namespace uncxx
} // namespace revenant

llvm::PreservedAnalyses
revenant::UncxxPass::run(llvm::Function &F, llvm::FunctionAnalysisManager &) {
  Attribute User = F.getFnAttribute("revenant.user");
  if (!User.isStringAttribute() || User.getValueAsString() != "true")
    return llvm::PreservedAnalyses::all();
  if (UserNames && !UserNames->contains(F.getName()))
    return llvm::PreservedAnalyses::all();
  if (Metadata *Language = F.getParent()->getModuleFlag("revenant.lang")) {
    auto *Name = dyn_cast<MDString>(Language);
    if (!Name || Name->getString() != "cpp")
      return llvm::PreservedAnalyses::all();
  }
  CxxFlavor Fl = Flavor;
  if (Fl == CxxFlavor::Auto) {
    // The flavor is a property of the module's symbols; inferring it walks
    // every function and global name, so it is done once per module.
    if (!FlavorResolved || CachedModule != F.getParent()) {
      CachedFlavor = uncxx::flavorOf(*F.getParent());
      CachedModule = F.getParent();
      FlavorResolved = true;
    }
    Fl = CachedFlavor;
  }
  unsigned N = 0;
  const llvm::DataLayout &DL = F.getParent()->getDataLayout();
  // Block idioms first: they consume calls (grow/reallocate) the scalar
  // folder must not see as plain code.
  N += uncxx::runBlockIdioms(F, DL, Fl);
  N += uncxx::runScalarIdioms(F, DL, Fl);
  // One more block round: scalar folds (size/data) can complete guard
  // shapes the first block round could not yet prove.
  N += uncxx::runBlockIdioms(F, DL, Fl);
  if (!N)
    return llvm::PreservedAnalyses::all();
  return llvm::PreservedAnalyses::none();
}

// Plugin entry for `opt -load-pass-plugin`. The `revenant-opt`
// superbuild (owner: MuseTxPipeline) links this library statically and
// registers the `revenant-uncxx` name in Pipeline/Register.cpp, so the
// entry point is compiled only for the standalone plugin DLL: a static
// link of every transform would otherwise export llvmGetPassPluginInfo
// more than once. Windows needs the explicit dllexport; without it opt
// fails with "Plugin entry point not found".
#ifdef REVENANT_BUILD_PLUGIN

#if defined(_WIN32)
#define REVENANT_PLUGIN_EXPORT __declspec(dllexport)
#else
#define REVENANT_PLUGIN_EXPORT
#endif

extern "C" REVENANT_PLUGIN_EXPORT ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "RevenantUncxx", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &PM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "revenant-uncxx") {
                    PM.addPass(revenant::UncxxPass());
                    return true;
                  }
                  return false;
                });
          }};
}
#endif
