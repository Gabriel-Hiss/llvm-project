//===- Lane0.h - lane-0 scalar web detection --------------------*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// `lane0` (first half of the `crates/unopt` port): a vector web where every
// use reads only lane 0 (`extractelement v, 0`) or forwards the whole
// vector transparently (`phi`, vector `bitcast`, `insertelement v, s, 0`
// base) collapses to a scalar web of the element type.
//
// Mirrors `crates/unopt/src/lane0.rs`:
// - `representable`: defs whose lane 0 has a scalar form (phi, load,
//   128-bit bitcast, scalar zext/sext, insertelement-lane0).
// - `shrink`: greatest fixpoint, drop members with a poison use (any use
//   that is not a lane-0 extract/trunc or a forward into another member)
//   or a non-member 128-bit input.
// - `webs` + `web_elem_ty`: transparent-use connected components agree on
//   one element type; mixed layouts stay full width.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_DEVECTORIZE_LANE0_H
#define REVENANT_TRANSFORMS_DEVECTORIZE_LANE0_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Type.h"

namespace revenant {
namespace lane0 {

// A use of a vector value.
enum class Role {
  Scalar,      // reads lane 0 only: extractelement(v, 0), trunc to <=64
  Transparent, // forwards lane-ness: phi, bitcast to 128, insertelement base
  Poison,      // anything else keeps full width
};

// Classify how instruction `User` reads vector value `V`.
Role roleOf(const llvm::Instruction *User, const llvm::Value *V);

// Defs whose lane 0 has a scalar form. Matches `representable` in the
// reference (`lane0.rs`): phi, load, 128-bit bitcast, scalar zext/sext,
// insertelement-lane0.
bool isRepresentable(const llvm::Instruction *Def);

// `insertelement v, s, 0` -> (element width bits, inserted scalar).
// Absent for any other instruction (non-zero lane, non-constant lane).
std::optional<std::pair<unsigned, llvm::Value *>>
insertParts(const llvm::Instruction *I);

// A web member may only read members (or vector constants / undef); a
// full-width producer feeding it would otherwise get narrowed.
bool inputsScalarized(const llvm::Instruction *Def,
                      const llvm::DenseSet<const llvm::Value *> &Lane0);

// Drop members until every use reads lane 0 (or forwards into another
// member) and every vector input of a member is itself a member.
void shrink(llvm::Function &F,
            const llvm::DenseMap<const llvm::Value *,
                                 const llvm::Instruction *> &Defs128,
            const llvm::DenseMap<const llvm::Value *,
                                 llvm::SmallVector<const llvm::Instruction *, 4>>
                &Users,
            llvm::DenseSet<const llvm::Value *> &Lane0);

// Connected components of members linked by transparent uses.
llvm::SmallVector<llvm::SmallVector<const llvm::Value *, 8>, 4>
webs(const llvm::DenseMap<const llvm::Value *,
                          llvm::SmallVector<const llvm::Instruction *, 4>>
         &Users,
     const llvm::DenseSet<const llvm::Value *> &Lane0);

// The single element type every extract/insert in `Web` agrees on
// (64-bit int when nothing constrains it), or null when layouts disagree
// or some def/consumer would need more than one cast.
llvm::Type *webElemTy(llvm::Function &F,
                      const llvm::DenseMap<const llvm::Value *,
                                           const llvm::Instruction *> &Defs128,
                      const llvm::DenseMap<const llvm::Value *,
                                           llvm::SmallVector<const llvm::Instruction *, 4>>
                          &Users,
                      const llvm::SmallVector<const llvm::Value *, 8> &Web);

// Apply the scalarization to F. Returns the rewrite count.
unsigned propagate(llvm::Function &F);

} // namespace lane0
} // namespace revenant

#endif
