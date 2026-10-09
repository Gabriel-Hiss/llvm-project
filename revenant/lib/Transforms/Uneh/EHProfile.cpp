//===- EHProfile.cpp - local compiler EH habits ------------------------===//
//
// Part of the Revenant LLVM fork.
//
// See EHProfile.h. Name evidence: MSVC/clang-cl `?catch$N`/`?dtor$N`/
// `?fin$N`/`?filt$N` funclets and `$cppxdata$`/`$tryMap$`/`$handlerMap$`/
// `$ip2state$` tables in cpp-o0/cpp-clang-o0 .map + main.cod; MinGW
// `__gxx_personality_seh0` + `.gcc_except_table` in cpp-gcc-o0.map;
// rustc `__CxxFrameHandler3` personality + `cleanuppad`/`drop_glue`
// shapes in rust-o0/artifacts/corpus_rust.pre.ll (no `rust_eh_personality`
// import anywhere in the rust-o0 map: confirmed from the binary).
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/Uneh/EHProfile.h"

namespace revenant::uneh {

llvm::StringRef personalityName(Producer P) {
  switch (P) {
  case Producer::Msvc:
  case Producer::ClangCl:
    return "__CxxFrameHandler3";
  case Producer::MinGwGcc:
    return "__gxx_personality_seh0";
  case Producer::Rustc:
    // rustc on Windows lowers to the MSVC-compatible handler (observed:
    // every personality in rust-o0 pre-IR is __CxxFrameHandler3; the map
    // imports __CxxFrameHandler3 + __C_specific_handler from VCRUNTIME).
    return "__CxxFrameHandler3";
  case Producer::Unknown:
    return "";
  }
  return "";
}

bool isThrowRoutine(llvm::StringRef CalleeName) {
  // Throw sites start unwinding; they keep plain calls (pre-IR shows
  // `invoke void @_CxxThrowException ... to label %31 unwind label %17`:
  // the throw itself unwinds to the same dispatch, it is not converted
  // into a dispatch of its own).
  return CalleeName == "_CxxThrowException" || CalleeName == "__cxa_throw" ||
         CalleeName == "__cxa_rethrow" ||
         CalleeName.contains("panicking::panic") ||
         CalleeName.contains("panic_fmt") ||
         CalleeName.contains("panic_bounds_check") ||
         CalleeName.contains("panic_const");
}

HandlerKind handlerKindForName(llvm::StringRef Name, Producer P) {
  if (P == Producer::Msvc || P == Producer::ClangCl ||
      P == Producer::Rustc || P == Producer::Unknown) {
    // `?catch$N@?0??f@@...` / backtick-quoted `` `f'::`1'::catch$N ``.
    if (Name.contains("catch$"))
      return HandlerKind::Catch;
    // `?dtor$N@...`, `main$dtor$N`, `?fin$N@...` (SEH finally funclets are
    // `?fin$N@0@f@@`).
    if (Name.contains("dtor$") || Name.contains("fin$"))
      return HandlerKind::Cleanup;
    // `?filt$N@...` SEH filter expressions stay opaque asm calls.
    if (Name.contains("filt$"))
      return HandlerKind::Filter;
  }
  if (P == Producer::MinGwGcc || P == Producer::Unknown) {
    if (Name == "__cxa_begin_catch" || Name == "__cxa_end_catch")
      return HandlerKind::Catch;
  }
  return HandlerKind::None;
}

TableLayout tableLayoutFor(Producer P) {
  switch (P) {
  case Producer::Msvc:
  case Producer::ClangCl:
    return TableLayout::FuncInfo;
  case Producer::MinGwGcc:
    return TableLayout::LSDA;
  case Producer::Rustc:
    return TableLayout::Cleanup;
  case Producer::Unknown:
    return TableLayout::None;
  }
  return TableLayout::None;
}

} // namespace revenant::uneh
