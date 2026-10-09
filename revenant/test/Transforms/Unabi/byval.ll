; Struct-by-pointer back to byval: on Win64 clang passes large structs as
; plain pointers (no byval marker, the callee copies); ir-pre shows the
; same ptr shape, so the decompiler-level source shape carries byval to say
; the callee owns a copy. The annotation marks V<> params byval.
; Corpus: audio-nodegraph-clang-o1 ?setNodeApp (std::string params).
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unabi -verify-each -S %s | %FileCheck %s

%struct.String = type { ptr, i64, i64 }

; CHECK-LABEL: define i1 @take(ptr %0, ptr %1)
define i1 @take(ptr %0, ptr %1) #0 {
entry:
  %l = load i64, ptr %1, align 8, !revenant.va !{i64 7028}
  %z = icmp eq i64 %l, 0, !revenant.va !{i64 7032}
  ret i1 %z
}

attributes #0 = { "revenant.sig"="T V%struct.String => r8" }
