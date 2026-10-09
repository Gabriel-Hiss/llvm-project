/*===-- revenant-c/revenant.h - Revenant C ABI for the GPUI app ---*- C -*-===*\
|*
|* Stable C interface over the decompiler pipeline, mirroring crates/engine:
|* open a binary, list functions, disassemble one function, decompile one
|* function to source with a VA line map.
|*
|* All returned char* / struct results are heap-allocated and released with
|* the matching rv_*_free call. Strings are UTF-8. Session calls are not
|* thread-safe; create one session per thread.
|*
\*===----------------------------------------------------------------------===*/

#ifndef REVENANT_C_REVENANT_H
#define REVENANT_C_REVENANT_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define RV_API __declspec(dllexport)
#else
#define RV_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Source languages the decompiler can emit. */
typedef enum rv_lang {
  RV_LANG_C = 0,
  RV_LANG_CPP = 1,
  RV_LANG_RUST = 2,
} rv_lang;

/* Producer compiler detected for the binary. */
typedef enum rv_compiler {
  RV_COMPILER_MSVC = 0,
  RV_COMPILER_CLANG_CL = 1,
  RV_COMPILER_RUSTC = 2,
  RV_COMPILER_MINGW_GCC = 3,
  RV_COMPILER_UNKNOWN = 4,
} rv_compiler;

typedef struct rv_session rv_session;
typedef struct rv_func_list rv_func_list;
typedef struct rv_asm rv_asm;
typedef struct rv_decompiled rv_decompiled;

/* One discovered function. */
typedef struct rv_function_info {
  uint64_t va;          /* start VA */
  uint64_t end;         /* end VA (max block end) */
  uint32_t instr_count; /* decoded instruction count, 0 when unknown */
  uint32_t block_count; /* basic block count, 0 when unknown */
  const char *name;     /* owned by the list; valid until rv_func_list_free */
} rv_function_info;

/* One output-line -> VA row of the decompile line map. */
typedef struct rv_line_va {
  uint32_t line; /* 1-based line of emitted text */
  uint64_t va;   /* first asm VA the line's statement came from */
} rv_line_va;

/* Last-error string for the calling thread. Valid until the next rv_* call
 * on this thread. Never NULL. */
RV_API const char *rv_last_error(void);

/* Open a PE32+ image and run session setup (language detection, function
 * discovery). Returns NULL on failure; see rv_last_error. */
RV_API rv_session *rv_open(const char *path);
RV_API void rv_close(rv_session *s);

/* Detected producer language/compiler, from crates/detect evidence
 * (no PDB pass yet: symbols, imports and embedded strings only). */
RV_API rv_lang rv_detected_lang(const rv_session *s);
RV_API rv_compiler rv_detected_compiler(const rv_session *s);
/* Free with rv_string_free. */
RV_API char *rv_detected_compiler_version(const rv_session *s);
RV_API char *rv_detect_evidence(const rv_session *s);

RV_API uint64_t rv_image_base(const rv_session *s);
RV_API uint64_t rv_entry_point(const rv_session *s);

/* Every discovered function, sorted by start VA. */
RV_API rv_func_list *rv_functions(const rv_session *s);
RV_API size_t rv_func_list_len(const rv_func_list *l);
RV_API const rv_function_info *rv_func_list_get(const rv_func_list *l,
                                                size_t i);
RV_API void rv_func_list_free(rv_func_list *l);

/* Intel-syntax disassembly of the function starting at `va`.
 * Returns NULL when `va` is not a function start; see rv_last_error.
 * rv_asm_text is borrowed; valid until rv_asm_free. */
RV_API rv_asm *rv_disasm_function(const rv_session *s, uint64_t va);
RV_API const char *rv_asm_text(const rv_asm *a);
RV_API void rv_asm_free(rv_asm *a);

/* Decompile the function at `va` to `lang` source text with a VA line map.
 * Returns NULL on failure; see rv_last_error.
 * The line map is empty when the source emitter cannot attribute lines
 * yet (e.g. the Clang frontend pre-VA-map path); the text is still valid. */
RV_API rv_decompiled *rv_decompile(const rv_session *s, uint64_t va,
                                   rv_lang lang);
RV_API const char *rv_decompiled_text(const rv_decompiled *d);
RV_API size_t rv_decompiled_line_count(const rv_decompiled *d);
RV_API const rv_line_va *rv_decompiled_line_map(const rv_decompiled *d);
RV_API void rv_decompiled_free(rv_decompiled *d);

RV_API void rv_string_free(char *s);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* REVENANT_C_REVENANT_H */
