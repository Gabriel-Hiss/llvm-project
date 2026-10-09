; Struct-by-pointer stays a plain ptr: on Win64 clang passes large structs
; as plain pointers (ir-pre carries no byval marker; the corpus has zero
; byval attributes), so V<> only renames/restores the receiver and validates
; the named struct exists. Callee-copy ownership stays with sigs (open
; question with MuseSigs2: the slice plugin cannot add param attributes).
; Corpus: audio-nodegraph-clang-o1 ?setNodeApp (std::string params).
; RUN: %revenant-opt -passes=revenant-unabi -verify-each -S %s | %FileCheck %s

%struct.String = type { ptr, i64, i64 }

; CHECK-LABEL: define i32 @take(ptr %this, ptr %a1)
; CHECK: ret i32
define i32 @take(i64 %0, ptr %1) #0 {
entry:
  %p = inttoptr i64 %0 to ptr, !revenant.va !{i64 7024}
  %l = load i64, ptr %1, align 8, !revenant.va !{i64 7028}
  %t = trunc i64 %l to i32, !revenant.va !{i64 7032}
  %slot = alloca %struct.String, align 8
  store ptr %1, ptr %slot, align 8
  ret i32 %t
}

attributes #0 = { "revenant.sig"="T V%struct.String => r32" }
