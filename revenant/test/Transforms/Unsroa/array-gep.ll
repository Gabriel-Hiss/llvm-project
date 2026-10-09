; RUN: %revenant-opt -passes=revenant-unsroa -S %s | %FileCheck %s
;
; Corpus: c-clang-o1, @main. A byte GEP at an array element boundary off a
; `[16 x i32]` alloca becomes the array element GEP the ir-pre had.

target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"

define i32 @main_vec() {
; CHECK-LABEL: define i32 @main_vec(
bb:
  %vec = alloca [16 x i32], align 16, !revenant.va !{i64 24576}
  %elt = getelementptr inbounds i8, ptr %vec, i64 12, !revenant.va !{i64 24577}
; CHECK: getelementptr inbounds [16 x i32], ptr %vec, i64 0, i64 3
  %v = load i32, ptr %elt, align 4, !revenant.va !{i64 24578}
  ret i32 %v
}
