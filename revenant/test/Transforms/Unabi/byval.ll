; Struct-by-pointer stays a plain ptr: on Win64 clang passes large structs
; as plain pointers (ir-pre carries no byval marker; the corpus has zero
; byval attributes), so V<> only renames/restores the receiver and validates
; the named struct exists. Callee-copy ownership stays with sigs (open
; question with MuseSigs2: the slice plugin cannot add param attributes).
; Corpus: audio-nodegraph-clang-o1 ?setNodeApp (std::string params).
; RUN: %revenant-opt -passes=revenant-unabi -verify-each -S %s | %FileCheck %s

%struct.String = type { ptr, i64, i64 }

; CHECK-LABEL: define i8 @take(ptr %this, ptr %a1)
define i64 @take(i64 %0, i64 %1) #0 {
entry:
  %p = inttoptr i64 %1 to ptr, !revenant.va !{i64 7024}
  %l = load i64, ptr %p, align 8, !revenant.va !{i64 7028}
  %z = icmp eq i64 %l, 0, !revenant.va !{i64 7032}
  %z8 = zext i1 %z to i8, !revenant.va !{i64 7036}
  %e = zext i8 %z8 to i64, !revenant.va !{i64 7040}
  ret i64 %e
}

attributes #0 = { "revenant.sig"="T V%struct.String => r8" }
