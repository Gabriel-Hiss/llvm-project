; Varargs `...` and ctor/dtor this-returns: lifted varargs lack the `...`
; bit (sigs only recovers the fixed prefix), and lifted ctors return their
; own receiver in rax. Corpus: c-clang-o2 @printf(ptr, ...) and
; cpp-clang-o2 runtime_error ctor @"??0runtime_error@std@@QEAA@PEBD@Z"
; (ptr returned, ptr %1) -> void.
; RUN: %revenant-opt -passes=revenant-unabi -verify-each -S %s | %FileCheck %s

; CHECK-LABEL: define i32 @myprintf(ptr %a0, ptr %a1, ...)
define i32 @myprintf(ptr %0, ptr %1) #0 {
entry:
  %r = call i32 (ptr, ...) @callee(ptr %0, ptr %1)
  ret i32 %r
}

declare i32 @callee(ptr, ...)

; CHECK-LABEL: define void @ctor(ptr %a0, ptr %a1)
define ptr @ctor(ptr %0, ptr %1) #1 {
entry:
  store ptr %1, ptr %0, align 8, !revenant.va !{i64 8000}
  ret ptr %0
}

; CHECK-LABEL: define void @user(ptr %o, ptr %msg)
; CHECK: call void @ctor(ptr %o, ptr %msg)
; CHECK: ret void
define void @user(ptr %o, ptr %msg) {
  %r = call ptr @ctor(ptr %o, ptr %msg)
  ret void
}

attributes #0 = { "revenant.sig"="p p ... => r32" }
attributes #1 = { "revenant.sig"="p p => t" }
