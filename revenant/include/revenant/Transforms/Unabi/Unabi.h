//===- Unabi.h - undo Win64 ABI lowering -------------------------*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// Reverses Win64 ABI lowering on lifted functions so their signatures match
// the source instead of the binary: integer register width restores (undoing
// sigs over-approximation and optimizer argument promotion), i64 back to
// ptr, sret slots back to struct returns, RAX-coerced small structs back to
// struct returns, struct-by-pointer back to byval, implicit `this` on C++
// methods, ctor/dtor this-returns back to void, and `...` on varargs.
//
// Side input is one string attribute per function, `"revenant.sig"`, whose
// grammar is the flat Signature serialization agreed with LunaSigs plus
// source-shape extensions:
//
//   sig    := params ("=>" ret)?
//   params := tok* ("..." marks a vararg function, must be last)
//   tok    := r8 | r16 | r32 | r64            integer param, N bits
//             (i8/i16/i32/i64 accepted as aliases)
//           | f32 | f64                       float/double param
//           | p                               pointer param
//           | s0x<off>                        stack-slot param (positional
//             no-op: the offset only documents the home-space slot)
//           | T                               `this`: param 0 of a method,
//             becomes a ptr named `this`
//           | S<name>                         sret slot: removed from the
//             params, returned as struct `name` instead
//           | V<name>                         struct passed by pointer:
//             ptr with byval(`name`)
//   ret    := v                               void
//           | r8 | r16 | r32 | r64 | f32 | f64 | p
//           | S<name>                         struct return (pairs with an
//             S<name> slot param)
//           | P<name>                         RAX-coerced small struct:
//             current i64 (or equal-sized) return becomes struct `name`
//           | t                               ctor/dtor: every return hands
//             back param 0, becomes void
//
// `name` is the LLVM struct name as in getTypeByName (no quotes), and the
// type must be defined in the module or the function is left untouched.
// Positional rule: param tokens (excluding `...`) must match the current
// parameter count exactly, anything else is left untouched.
//
// Trust model: the attribute is trusted the way sigs are trusted. Value
// rewrites (narrowing) assume the annotation names the true source type;
// shape rewrites (sret, pair bitcopy, this) are guarded on the IR itself:
// the sret slot may only feed stores, a `t` return must provably hand back
// param 0, pair bitcopies require equal store sizes, and any indirect use,
// musttail call, callbr, intrinsic, opaque stub, or pre-existing sret/byval
// bails the function out. Consumed attributes are removed on success and
// kept on no-fire.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_UNABI_UNABI_H
#define REVENANT_TRANSFORMS_UNABI_UNABI_H

#include "llvm/IR/PassManager.h"

namespace revenant {

/// Module pass: changing a signature needs a new Function object, which a
/// FunctionPassManager pass cannot erase safely mid-iteration.
struct UnabiPass : llvm::PassInfoMixin<UnabiPass> {
  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace revenant

#endif
