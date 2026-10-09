//===- EHProfile.h - local compiler EH habits ----------------*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// Minimal local compiler profile for exception handling, owned by
// revenant-uneh as a fallback until LunaCompilerProfile's
// `revenant/Compiler/Profile.h` (`profileFor`, `ehModel()`) lands, at
// which point this file consolidates onto it (its owner takes the merge).
// Only string-level habits live here: personality symbol names, funclet
// name recognition, and the table-layout tag. Detection (which producer
// built the binary) stays with the profile owner; this file only answers
// "given producer P, what are its EH habits".
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_UNEH_EHPROFILE_H
#define REVENANT_TRANSFORMS_UNEH_EHPROFILE_H

#include "llvm/ADT/StringRef.h"

namespace revenant::uneh {

/// Which toolchain's EH tables govern a function. Mirrors the detected
/// producer (owner: LunaCompilerProfile); `Unknown` takes the generic
/// LLVM-backend path (Itanium-style landing pads).
enum class Producer {
  Msvc,      // cl: __CxxFrameHandler3/4, __C_specific_handler, FuncInfo maps
  ClangCl,   // clang-cl: same table shapes as MSVC
  MinGwGcc,  // GCC/MinGW: __gxx_personality_seh0, .gcc_except_table LSDA
  Rustc,     // rustc (panic=unwind): cleanuppads, drop glue
  Unknown,   // generic fallback: LLVM-backend shapes only
};

/// Table layout backing a function's EH regions.
enum class TableLayout {
  None,    // nounwind / panic=abort: no tables, no EH to rebuild
  FuncInfo,// MSVC/clang-cl $cppxdata/$tryMap/$handlerMap/$ip2state (+SEH)
  LSDA,    // MinGW .gcc_except_table language-specific data area
  Cleanup, // Rust unwind cleanups (drop glue), no catch tables
};

/// Class of an out-of-line funclet/symbol name.
enum class HandlerKind {
  None,   // not an EH handler
  Catch,  // `?catch$N@...`, MinGW catch action, SEH __except body
  Cleanup,// `?dtor$N@...`, `main$dtor$N`, `?fin$N@...`, finally body
  Filter, // `?filt$N@...` SEH filter expression (stays opaque asm)
};

/// Personality symbol for a producer. Empty string when the producer has
/// no single personality (Unknown: inferred from the binary's imports).
llvm::StringRef personalityName(Producer P);

/// True for the runtime entry points that throw (never become invoke
/// unwind edges themselves; they start unwinding).
bool isThrowRoutine(llvm::StringRef CalleeName);

/// Classify an out-of-line funclet/symbol name for producer P.
HandlerKind handlerKindForName(llvm::StringRef Name, Producer P);

/// Table layout backing producer P's EH regions.
TableLayout tableLayoutFor(Producer P);

} // namespace revenant::uneh

#endif
