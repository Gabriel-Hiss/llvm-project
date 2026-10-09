//===- TypeRecovery.h - Recovered types/names as DI metadata ----*- C++ -*-===//
//
// Part of the Revenant project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// TypeRecovery (pass name `revenant-types`): a FunctionPassManager pass that
// runs post-structure, pre-emission. It walks the lifted function, infers
// signedness and pointer/struct shape from IR use (the port of the
// signedness pre-scan in the reference `crates/structure` plus the
// load/store/GEP typing the structurer does while emitting expressions),
// and writes the recovered types and names as standard debug-info metadata
// so the Clang/rustc frontends read them like real debug info:
//
//   - integer locals: DIBasicType (signed vs unsigned from compare/divide/
//     shift/cast evidence)
//   - pointer locals: DIDerivedType pointer chains
//   - aggregate locals: DICompositeType structs inferred from constant-offset
//     field access patterns through one base pointer
//   - names: DILocalVariable entries attached with llvm.dbg.declare records
//
// The pass never changes code: no instruction is added, removed, or
// retargeted. Debug metadata and dbg.declare records are the only output.
// It preserves `!revenant.region` structure annotations, `!revenant.va`
// provenance, and `!revenant.opaque` markers, and the emitted `.ll` stays
// parseable by LLVM 22.
//
// DI plumbing comes from RevenantIR (`revenant/IR/Contract.h`, owned by
// RvIRContract); this pass uses only those helpers plus raw DIBuilder type
// nodes, so there is a single writer of module/CU/subprogram plumbing.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_TYPERECOVERY_TYPERECOVERY_H
#define REVENANT_TRANSFORMS_TYPERECOVERY_TYPERECOVERY_H

#include "llvm/IR/PassManager.h"

namespace revenant {

struct TypeRecoveryPass : public llvm::PassInfoMixin<TypeRecoveryPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);

  static bool isRequired() { return true; }
};

} // namespace revenant

#endif // REVENANT_TRANSFORMS_TYPERECOVERY_TYPERECOVERY_H
