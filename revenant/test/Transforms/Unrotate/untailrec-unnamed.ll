; RUN: opt -load-pass-plugin=%shlibdir/RevenantTransformUnrotate.dll -passes=revenant-untailrec -verify-each -S %s | FileCheck %s
; Numeric SSA values have empty instruction names. Clone matching must not
; confuse two unnamed sexts or place a header user before its latch clone.
define i32 @unnamed(i32 %a, i32 %b) {
entry:
  %guard = icmp eq i32 %b, 0
  br i1 %guard, label %exit, label %loop
loop:
  %0 = phi i32 [ %a, %entry ], [ %1, %loop ]
  %1 = phi i32 [ %b, %entry ], [ %5, %loop ]
  %2 = sext i32 %0 to i64
  %3 = sext i32 %1 to i64
  %4 = srem i64 %2, %3
  %5 = trunc i64 %4 to i32
  %done = icmp eq i32 %5, 0
  br i1 %done, label %exit2, label %loop
exit:
  ret i32 %a
exit2:
  ret i32 %1
}
; CHECK-LABEL: define i32 @unnamed
; CHECK: call i32 @unnamed
; CHECK: ret i32

; The dependencies of two recursive arguments overlap. Header instruction
; order, not reversed DFS discovery, defines their dominance order.
define i32 @shared(i32 %a, i32 %b) {
entry:
  %guard = icmp eq i32 %b, 0
  br i1 %guard, label %exit, label %loop
loop:
  %pa = phi i32 [ %a, %entry ], [ %nexta, %latch ]
  %pb = phi i32 [ %b, %entry ], [ %nextb, %latch ]
  %base = add i32 %pa, %pb
  %left = add i32 %base, 1
  %right = add i32 %base, 2
  %nexta = xor i32 %left, %right
  %nextb = add i32 %left, %nexta
  %done = icmp eq i32 %nextb, 0
  br i1 %done, label %exit2, label %latch
latch:
  br label %loop
exit:
  ret i32 %a
exit2:
  ret i32 %pb
}
; CHECK-LABEL: define i32 @shared
; CHECK: call i32 @shared
; CHECK: ret i32
