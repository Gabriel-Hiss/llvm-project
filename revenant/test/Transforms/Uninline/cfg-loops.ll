; Controlled CFG proof regressions. Corpus null-check fixtures are in cfg.ll
; and cfg-effects.ll; this file specifically checks cyclic PHI references.
; RUN: %revenant-opt -passes=revenant-uninline,verify -S %s | %FileCheck %s

define i32 @sum_loop(ptr %p, i32 %n) {
entry:
  %positive = icmp sgt i32 %n, 0
  br i1 %positive, label %loop, label %exit
loop:
  %index = phi i32 [ 0, %entry ], [ %next, %loop ]
  %sum = phi i32 [ 0, %entry ], [ %added, %loop ]
  %address = getelementptr i32, ptr %p, i32 %index
  %value = load i32, ptr %address, align 4
  %added = add i32 %sum, %value
  %next = add i32 %index, 1
  %done = icmp eq i32 %next, %n
  br i1 %done, label %exit, label %loop
exit:
  %result = phi i32 [ 0, %entry ], [ %added, %loop ]
  ret i32 %result
}

; CHECK-LABEL: define i32 @loop_caller(
; CHECK: call i32 @sum_loop(ptr %p, i32 %n)
; CHECK-NEXT: br label %uninline.cont
; CHECK-NOT: phi
; CHECK: uninline.cont:
; CHECK: ret i32
define i32 @loop_caller(ptr %p, i32 %n) {
entry:
  %positive.i = icmp sgt i32 %n, 0
  br i1 %positive.i, label %loop.i, label %exit.i
loop.i:
  %index.i = phi i32 [ %next.i, %loop.i ], [ 0, %entry ]
  %sum.i = phi i32 [ %added.i, %loop.i ], [ 0, %entry ]
  %address.i = getelementptr i32, ptr %p, i32 %index.i
  %value.i = load i32, ptr %address.i, align 4
  %added.i = add i32 %sum.i, %value.i
  %next.i = add i32 %index.i, 1
  %done.i = icmp eq i32 %next.i, %n
  br i1 %done.i, label %exit.i, label %loop.i
exit.i:
  %result.i = phi i32 [ 0, %entry ], [ %added.i, %loop.i ]
  ret i32 %result.i
}

; Different overflow behavior cannot be matched, even with the same CFG.
; CHECK-LABEL: define i32 @different_flags(
; CHECK-NOT: call i32 @sum_loop
; CHECK: add nsw i32
; CHECK: ret i32
define i32 @different_flags(ptr %p, i32 %n) {
entry:
  %positive.i = icmp sgt i32 %n, 0
  br i1 %positive.i, label %loop.i, label %exit.i
loop.i:
  %index.i = phi i32 [ 0, %entry ], [ %next.i, %loop.i ]
  %sum.i = phi i32 [ 0, %entry ], [ %added.i, %loop.i ]
  %address.i = getelementptr i32, ptr %p, i32 %index.i
  %value.i = load i32, ptr %address.i, align 4
  %added.i = add nsw i32 %sum.i, %value.i
  %next.i = add i32 %index.i, 1
  %done.i = icmp eq i32 %next.i, %n
  br i1 %done.i, label %exit.i, label %loop.i
exit.i:
  %result.i = phi i32 [ 0, %entry ], [ %added.i, %loop.i ]
  ret i32 %result.i
}
