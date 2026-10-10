; RUN: %revenant-opt -passes=revenant-unpeel -S %s | %FileCheck %s
; Guard + do-while rotation: block %G guards the loop condition and
; branches to %H (enter) or %X (exit). The loop's latch tests the same
; condition. After the fold, G is deleted and the entry goes straight to H.
; CHECK-LABEL: define i32 @gr(

define i32 @gr(i32 %n) {
Entry:
  br label %G
G:
  %c = icmp sgt i32 %n, 0, !revenant.va !{i64 4096}
  br i1 %c, label %H, label %X
H:
  %i = phi i32 [ 0, %G ], [ %in, %L ]
  %s = phi i32 [ 0, %G ], [ %sa, %L ]
  %sa = add i32 %s, 1, !revenant.va !{i64 4112}
  %in = add i32 %i, 1, !revenant.va !{i64 4113}
  %c2 = icmp slt i32 %in, %n, !revenant.va !{i64 4114}
  br i1 %c2, label %L, label %X
L:
  br label %H
; CHECK-NOT: G:
; CHECK: br label %H
X:
  %r = phi i32 [ 0, %G ], [ %sa, %H ]
  ret i32 %r
}
; A guard-shaped block that also computes values consumed inside the loop
; is not a peel: deleting it would leave the loop's uses dangling. The fold
; must decline and G must survive.
; CHECK-LABEL: define i32 @gr_feeds_loop(
;
define i32 @gr_feeds_loop(ptr %base, i32 %n) {
Entry:
  br label %G
G:
  %elem = getelementptr i32, ptr %base, i64 4
  %c = icmp sgt i32 %n, 0, !revenant.va !{i64 4096}
  br i1 %c, label %H, label %X
H:
  %i = phi i32 [ 0, %G ], [ %in, %L ]
  %v = load i32, ptr %elem
  %in = add i32 %i, %v, !revenant.va !{i64 4113}
  %c2 = icmp slt i32 %in, %n, !revenant.va !{i64 4114}
  br i1 %c2, label %L, label %X
L:
  br label %H
; CHECK: G:
; CHECK: %elem = getelementptr i32, ptr %base, i64 4
X:
  %r = phi i32 [ 0, %G ], [ %in, %H ]
  ret i32 %r
}
; A guard-shaped block with a side effect (store) is likewise not a peel.
; CHECK-LABEL: define void @gr_store(
;
define void @gr_store(ptr %p, ptr %sink, i32 %n) {
Entry:
  br label %G
G:
  store i32 7, ptr %sink
  %c = icmp sgt i32 %n, 0, !revenant.va !{i64 4096}
  br i1 %c, label %H, label %X
H:
  %i = phi i32 [ 0, %G ], [ %in, %L ]
  %in = add i32 %i, 1, !revenant.va !{i64 4113}
  %c2 = icmp slt i32 %in, %n, !revenant.va !{i64 4114}
  br i1 %c2, label %L, label %X
L:
  br label %H
; CHECK: G:
; CHECK: store i32 7, ptr %sink
X:
  ret void
}
