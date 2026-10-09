//===- TypeInference.h - Signedness/pointer/struct inference -*- C++ -*-===//
//
// Part of the Revenant project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_TYPERECOVERY_TYPEINFERENCE_H
#define REVENANT_TRANSFORMS_TYPERECOVERY_TYPEINFERENCE_H

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Value.h"

#include <cstdint>
#include <vector>

namespace llvm {
class Instruction;
}

namespace revenant {

// Pure inference over one function. Collects signedness votes, pointer
// votes, and constant-offset field accesses, then answers queries. No
// DIBuilder use; the pass turns answers into metadata.
class TypeInference {
public:
  explicit TypeInference(llvm::Function &Fn) : F(Fn) {}

  void collect();

  // Unsigned-sticky join: true when any unsigned witness voted on the
  // value's alias web.
  bool isUnsigned(const llvm::Value *V) const;
  bool hasSignedWitness(const llvm::Value *V) const;
  bool isPointer(const llvm::Value *V) const;
  unsigned intBits(const llvm::Value *V, unsigned Default) const;

  struct FieldCell {
    uint64_t SizeBits = 0;
    const llvm::DILocation *Loc = nullptr;
  };
  struct FieldAccess {
    llvm::Value *Base = nullptr;
    // Offset in bits -> widest access cell.
    llvm::MapVector<uint64_t, FieldCell> Fields;
    uint64_t MaxEndBits = 0;
  };
  const FieldAccess *fieldsFor(const llvm::Value *Base) const;

  const llvm::DenseMap<llvm::Value *, FieldAccess> &bases() const {
    return Bases;
  }

private:
  using Id = uint32_t;
  struct Node {
    llvm::Value *V = nullptr;
    unsigned UnsignedVotes = 0;
    unsigned SignedVotes = 0;
    unsigned PointerVotes = 0;
    unsigned IntVotes = 0;
    unsigned FloatVotes = 0;
    unsigned MaxIntBits = 0;
    unsigned MaxFloatBits = 0;
    explicit Node(llvm::Value *Val = nullptr) : V(Val) {}
  };

  llvm::Function &F;
  llvm::DenseMap<llvm::Value *, Id> Index;
  std::vector<Node> Nodes;
  mutable std::vector<Id> Parent;
  llvm::DenseMap<llvm::Value *, FieldAccess> Bases;

  Id ensureValue(llvm::Value *V);
  Id find(Id N) const;
  void unite(llvm::Value *A, llvm::Value *B);
  void noteUse(const llvm::Value *V);
  void voteUnsigned(llvm::Value *V);
  void voteSigned(llvm::Value *V);
  void votePointer(llvm::Value *V);
  void voteInt(llvm::Value *V, unsigned Bits);
  void voteFloat(llvm::Value *V, unsigned Bits);
  void recordFieldAccess(llvm::Value *Ptr, uint64_t BaseOffset,
                         bool DirectMemOp, const llvm::Instruction *I);
};

} // namespace revenant

#endif // REVENANT_TRANSFORMS_TYPERECOVERY_TYPEINFERENCE_H
