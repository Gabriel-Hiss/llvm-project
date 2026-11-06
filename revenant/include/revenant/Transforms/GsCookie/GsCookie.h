//===- GsCookie.h - remove the MSVC /GS stack cookie sequence --------*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// MSVC's /GS option makes the compiler emit a stack cookie: the
// prologue xors the global `__security_cookie` with the stack pointer
// and stores the result in the frame, the epilogue reloads it, xors it
// with the stack pointer again and compares it with the global. A
// mismatch calls `__security_check_cookie`, which aborts.
//
//   prologue                          epilogue
//   %rsp  = ptrtoint ...              %v   = load  i64, ptr %slot
//   %c    = xor i64 %cookie, %rsp     %v2  = xor i64 %v, %rsp
//   store i64 %c, ptr %slot           %cmp = icmp eq i64 %cookie, %v2
//                                     br i1 %cmp, label %ok, label %fail
//                                     fail:
//                                       call void @__security_check_cookie(i64 %v2)
//
// The compiler's own pre-optimization IR has none of this: the backend
// inserts the cookie after the frontend has produced the IR, so the
// pre shape of the same function carries no cookie at all. The lifted
// IR keeps it, so the deoptimized IR differs from the compiler's.
//
// This pass removes the sequence: the prologue store and the epilogue
// compare, the branch and the `__security_check_cookie` call. The
// failure edge is folded away (the check never returns: it calls
// `__report_gsfailure`, which aborts), so the function keeps exactly
// the behaviour the compiler's IR describes.
//
// The removal leaves a marker: the function carries the string
// attribute `"rv.gscookie"="1"`, so a consumer (the source emitter) can
// tell that the function had a /GS cookie and drop whatever the
// cookie left behind in the frame.
//
// The pattern is matched by shape, never by name: the lifted callee is
// `sub_<VA>` (the security cookie helpers are not in the import table
// of a statically linked binary), so the sequence is recognized by the
// xor with the stack pointer, the compare against the cookie value and
// the call that takes the compared value.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_GSCOOKIE_GSCOOKIE_H
#define REVENANT_TRANSFORMS_GSCOOKIE_GSCOOKIE_H

#include "llvm/IR/PassManager.h"

namespace revenant {

struct GsCookiePass : public llvm::PassInfoMixin<GsCookiePass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

/// `revenant-stackprotector`: the cookie as the optimizing compiler's IR
/// right before instruction selection has it. The StackProtector pass leaves
/// `llvm.stackprotector(llvm.stackguard(), %StackGuardSlot)` at the entry of
/// a protected function and the selector expands the rest, so a function
/// whose cookie `revenant-gscookie` removed (`"rv.gscookie"`) gets those two
/// calls and the slot back. Unoptimized functions (`revenant.o0`) stay as
/// they are: the compiler's -O0 IR is compared before that pass ran. Only
/// the ir-final storage runs it; the source is built without the cookie.
struct StackProtectorPass : public llvm::PassInfoMixin<StackProtectorPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace revenant

#endif
