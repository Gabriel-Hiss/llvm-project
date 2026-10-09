//==- lib/CAPI/CAPI.cpp - C ABI over the engine ---------------------------==//
//
// Implements include/revenant-c/revenant.h on top of revenant::Session.
// Error strings go to a thread-local slot readable via rv_last_error.
//
//===----------------------------------------------------------------------===//

#include "revenant-c/revenant.h"
#include "revenant/Engine/Session.h"
#include "revenant/Object/PeFile.h"

#include <algorithm>
#include <new>
#include <string>
#include <vector>

#include "llvm/Support/Error.h"

struct rv_session {
  std::unique_ptr<revenant::Session> S;
};
struct rv_func_list {
  std::vector<rv_function_info> Infos;
  std::vector<std::string> Names;
};
struct rv_asm { std::string Text; };
struct rv_decompiled {
  std::string Text;
  std::vector<rv_line_va> Map;
};

using namespace revenant;

namespace {

thread_local std::string LastError;

void setError(const std::string &S) { LastError = S; }
void setError(llvm::Error E) {
  LastError = llvm::toString(std::move(E));
}
void clearError() { LastError.clear(); }

char *dupString(llvm::StringRef S) {
  char *Out = new char[S.size() + 1];
  std::copy(S.begin(), S.end(), Out);
  Out[S.size()] = '\0';
  return Out;
}

rv_lang toC(Lang L) {
  switch (L) {
  case Lang::C:
    return RV_LANG_C;
  case Lang::Cpp:
    return RV_LANG_CPP;
  case Lang::Rust:
    return RV_LANG_RUST;
  }
  return RV_LANG_C;
}

Lang fromC(rv_lang L) {
  switch (L) {
  case RV_LANG_C:
    return Lang::C;
  case RV_LANG_CPP:
    return Lang::Cpp;
  case RV_LANG_RUST:
    return Lang::Rust;
  }
  return Lang::C;
}

} // namespace

extern "C" {

RV_API const char *rv_last_error(void) { return LastError.c_str(); }

RV_API rv_session *rv_open(const char *Path) {
  clearError();
  if (!Path) {
    setError("null path");
    return nullptr;
  }
  auto SOrErr = Session::open(Path);
  if (!SOrErr) {
    setError(SOrErr.takeError());
    return nullptr;
  }
  auto *S = new (std::nothrow) rv_session;
  if (!S) {
    setError("out of memory");
    return nullptr;
  }
  S->S = std::move(*SOrErr);
  return S;
}

RV_API void rv_close(rv_session *S) { delete S; }

RV_API rv_lang rv_detected_lang(const rv_session *S) {
  return S ? toC(S->S->compiler().Language) : RV_LANG_C;
}

RV_API rv_compiler rv_detected_compiler(const rv_session *S) {
  if (!S)
    return RV_COMPILER_UNKNOWN;
  switch (S->S->compiler().Producer) {
  case Compiler::Msvc:
    return RV_COMPILER_MSVC;
  case Compiler::ClangCl:
    return RV_COMPILER_CLANG_CL;
  case Compiler::Rustc:
    return RV_COMPILER_RUSTC;
  case Compiler::MinGwGcc:
    return RV_COMPILER_MINGW_GCC;
  case Compiler::Unknown:
    return RV_COMPILER_UNKNOWN;
  }
  return RV_COMPILER_UNKNOWN;
}

RV_API char *rv_detected_compiler_version(const rv_session *S) {
  if (!S || !S->S->compiler().Version)
    return nullptr;
  return dupString(*S->S->compiler().Version);
}

RV_API char *rv_detect_evidence(const rv_session *S) {
  if (!S)
    return nullptr;
  std::string Out;
  for (const std::string &E : S->S->compiler().Evidence) {
    Out += E;
    Out += '\n';
  }
  return dupString(Out);
}

RV_API uint64_t rv_image_base(const rv_session *S) {
  return S ? S->S->pe().Img->Base.Value : 0;
}

RV_API uint64_t rv_entry_point(const rv_session *S) {
  return S ? S->S->pe().EntryPoint.Value : 0;
}

RV_API rv_func_list *rv_functions(const rv_session *S) {
  if (!S)
    return nullptr;
  auto *L = new (std::nothrow) rv_func_list;
  if (!L)
    return nullptr;
  const Program &P = S->S->program();
  L->Infos.reserve(P.Functions.size());
  L->Names.reserve(P.Functions.size());
  for (const auto &[Start, Rec] : P.Functions) {
    L->Names.push_back(Rec.Name);
    L->Infos.push_back({Start.Value, Rec.End.Value,
                        (uint32_t)Rec.InstrRvas.size(),
                        (uint32_t)Rec.Blocks.size(),
                        L->Names.back().c_str()});
  }
  return L;
}

RV_API size_t rv_func_list_len(const rv_func_list *L) {
  return L ? L->Infos.size() : 0;
}

RV_API const rv_function_info *rv_func_list_get(const rv_func_list *L,
                                                size_t I) {
  return L && I < L->Infos.size() ? &L->Infos[I] : nullptr;
}

RV_API void rv_func_list_free(rv_func_list *L) { delete L; }

RV_API rv_asm *rv_disasm_function(const rv_session *S, uint64_t Va_) {
  clearError();
  if (!S) {
    setError("null session");
    return nullptr;
  }
  auto Text = S->S->disasmFunction(Va(Va_));
  if (!Text) {
    setError(Text.takeError());
    return nullptr;
  }
  auto *A = new (std::nothrow) rv_asm;
  if (!A) {
    setError("out of memory");
    return nullptr;
  }
  A->Text = std::move(*Text);
  return A;
}

RV_API const char *rv_asm_text(const rv_asm *A) {
  return A ? A->Text.c_str() : "";
}

RV_API void rv_asm_free(rv_asm *A) { delete A; }

RV_API rv_decompiled *rv_decompile(const rv_session *S, uint64_t Va_,
                                   rv_lang Lang_) {
  clearError();
  if (!S) {
    setError("null session");
    return nullptr;
  }
  DecompileOpts Opts;
  Opts.Emit = EmitKind::Source;
  auto D = S->S->decompile(Va(Va_), fromC(Lang_), Opts);
  if (!D) {
    setError(D.takeError());
    return nullptr;
  }
  auto *Out = new (std::nothrow) rv_decompiled;
  if (!Out) {
    setError("out of memory");
    return nullptr;
  }
  Out->Text = std::move(D->Text);
  for (auto [Line, V] : Session::firstVaPerLine(D->LineVas))
    Out->Map.push_back({Line, V.Value});
  return Out;
}

RV_API const char *rv_decompiled_text(const rv_decompiled *D) {
  return D ? D->Text.c_str() : "";
}

RV_API size_t rv_decompiled_line_count(const rv_decompiled *D) {
  return D ? D->Map.size() : 0;
}

RV_API const rv_line_va *rv_decompiled_line_map(const rv_decompiled *D) {
  return D && !D->Map.empty() ? D->Map.data() : nullptr;
}

RV_API void rv_decompiled_free(rv_decompiled *D) { delete D; }

RV_API void rv_string_free(char *S) { delete[] S; }

} // extern "C"
