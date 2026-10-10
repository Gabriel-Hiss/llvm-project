; RUN: %revenant-opt -passes=revenant-unsroa -S %s | %FileCheck %s
;
; A callee-confirmed single-field run is not a struct: carving the window
; down to one field must bail, not index Fields[1] (SmallVector assert).
; Reduced from ripgrep-o2 sub_1400C3B40: the frame root has many offsets,
; but only one is confirmed by the callee, and the old code crashed here.
;
; CHECK-NOT: struct.agg

target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"

define i64 @callee(ptr %p) {
entry:
  %v = load i64, ptr %p, align 1
  ret i64 %v
}

define i64 @single_confirmed(ptr %arg) {
entry:
  %frame = alloca [64 x i8], align 1
  %a0 = getelementptr i8, ptr %frame, i64 8
  store i64 1, ptr %a0, align 1
  %a1 = getelementptr i8, ptr %frame, i64 32
  store i32 2, ptr %a1, align 1
  %r = call i64 @callee(ptr %a0)
  %v = load i64, ptr %a0, align 1
  %w = load i32, ptr %a1, align 1
  %s = add i64 %v, %r
  %t = zext i32 %w to i64
  %u = add i64 %s, %t
  ret i64 %u
}
