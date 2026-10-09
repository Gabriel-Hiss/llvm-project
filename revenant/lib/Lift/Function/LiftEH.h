//===- LiftEH.h - exception-region lifting for the function lifter - C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// Post-lift rewrite turning plain calls inside decoded EH try ranges into
// native LLVM EH: MSVC C++ try/catch as invoke/catchswitch/catchpad,
// __C_specific_handler SEH as in clang's Windows SEH lowering, unwind-map
// cleanups as cleanuppad, and MinGW LSDA sites as landingpad. Driven by
// revenant::decodeEHTables output for the lifted function's start VA.
//
// The lifter cannot emit invoke directly: the unwind destination is only
// known from the EH tables, so emitCall stays a plain call and this pass
// rewrites afterwards. Fires only on airtight matches: a call whose
// !revenant.va lies strictly inside a decoded try range, with a known
// handler or cleanup for that range. Anything else stays a call.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_LIFT_FUNCTION_LIFTEH_H
#define REVENANT_LIFT_FUNCTION_LIFTEH_H

#include "revenant/Analysis/EHTables.h"

namespace llvm {
class Function;
}

namespace revenant::lift {

/// Rewrite in-try calls of Fn into EH invokes using EH metadata for the
/// function starting at FuncStart. Returns the number of calls converted.
/// Never invents EH: with no matching FunctionEH entry returns 0.
unsigned liftEH(llvm::Function &Fn, const FunctionEH &EH);

} // namespace revenant::lift

#endif
