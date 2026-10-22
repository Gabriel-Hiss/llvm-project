; Phis that feed each other keep separate locals when both values are
; alive at once, and the copies on one edge run in an order that reads
; every old value before it is overwritten.
; RUN: %revenant-opt -passes=revenant-ssaout -S %s | %FileCheck %s

; Fibonacci step: x takes the old y while y takes x + y.
; CHECK-LABEL: define i32 @fib(
; CHECK-DAG: %v_x = alloca i32
; CHECK-DAG: %v_y = alloca i32
; CHECK: loop.loop_crit_edge:
; CHECK-NEXT: store i32 %i1, ptr %v_i
; CHECK-NEXT: %[[OLDY:.*]] = load i32, ptr %v_y
; CHECK-NEXT: store i32 %[[OLDY]], ptr %v_x
; CHECK-NEXT: store i32 %s, ptr %v_y
; CHECK-NEXT: br label %loop

define i32 @fib(i32 %n) {
entry:
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %i1, %loop ]
  %y = phi i32 [ 1, %entry ], [ %s, %loop ]
  %x = phi i32 [ 0, %entry ], [ %y, %loop ]
  %s = add i32 %x, %y
  %i1 = add i32 %i, 1
  %c = icmp slt i32 %i1, %n
  br i1 %c, label %loop, label %exit

exit:
  ret i32 %x
}

; A swap is a copy cycle: one old value goes through the spill local.
; CHECK-LABEL: define i32 @swap(
; CHECK-DAG: %v_spill = alloca i32
; CHECK-DAG: %v_x = alloca i32
; CHECK-DAG: %v_y = alloca i32
; CHECK: loop.loop_crit_edge:
; CHECK-NEXT: store i32 %i1, ptr %v_i
; CHECK-NEXT: %[[OLDX:.*]] = load i32, ptr %v_x
; CHECK-NEXT: store i32 %[[OLDX]], ptr %v_spill
; CHECK-NEXT: %[[OLDY:.*]] = load i32, ptr %v_y
; CHECK-NEXT: store i32 %[[OLDY]], ptr %v_x
; CHECK-NEXT: %[[SAVED:.*]] = load i32, ptr %v_spill
; CHECK-NEXT: store i32 %[[SAVED]], ptr %v_y
; CHECK-NEXT: br label %loop

define i32 @swap(i32 %n) {
entry:
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %i1, %loop ]
  %x = phi i32 [ 1, %entry ], [ %y, %loop ]
  %y = phi i32 [ 2, %entry ], [ %x, %loop ]
  %i1 = add i32 %i, 1
  %c = icmp slt i32 %i1, %n
  br i1 %c, label %loop, label %exit

exit:
  %r = sub i32 %x, %y
  ret i32 %r
}

; A nested-loop phi whose only use enters the inner loop does not
; interfere with the inner phi: both share one local.
; CHECK-LABEL: define i64 @nest(
; CHECK: %v_in = alloca i64
; CHECK-NOT: %v_o = alloca

define i64 @nest(i64 %n) {
entry:
  br label %outer

outer:
  %o = phi i64 [ 0, %entry ], [ %in.next, %olatch ]
  %k = phi i64 [ 0, %entry ], [ %k1, %olatch ]
  br label %inner

inner:
  %in = phi i64 [ %o, %outer ], [ %in.next, %inner ]
  %in.next = add i64 %in, 2
  %ic = icmp ult i64 %in.next, %n
  br i1 %ic, label %inner, label %olatch

olatch:
  %k1 = add i64 %k, 1
  %oc = icmp ult i64 %k1, %n
  br i1 %oc, label %outer, label %exit

exit:
  ret i64 %in.next
}
