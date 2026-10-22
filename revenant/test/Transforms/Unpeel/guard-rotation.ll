; RUN: %revenant-opt -passes=revenant-unpeel -S %s | %FileCheck %s
;
; undoGuardRotation may delete a guard block G only when the loop re-tests
; the same condition on the next IV step: G and the latch compare the same
; loop-invariant bound against the same IV at init/next step. Guard blocks
; whose condition the loop does not re-test are real branch arms and must
; survive.

; CHECK-LABEL: define i32 @gr(
; CHECK-NOT: {{^}}G:
; CHECK: br label %H
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
X:
  %r = phi i32 [ 0, %G ], [ %sa, %H ]
  ret i32 %r
}

; Same rotation; X's phi keeps only its latch incoming after the fold.
; Removing G's inedge walks the Exit-phi rewrite loop, which snapshots the
; phi list so removeIncomingValue can never free the iterated node (this
; loop previously dereferenced a freed phi, crash 0xfeeefeeefeeefeee).
; CHECK-LABEL: define i32 @gr_exitphi(
; CHECK-NOT: {{^}}G:
; CHECK: br label %H
; CHECK: {{^}}X:
; CHECK-NEXT: %r = phi i32 [ %i, %H ]
define i32 @gr_exitphi(i32 %n) {
Entry:
  br label %G
G:
  %c = icmp sgt i32 %n, 0, !revenant.va !{i64 4096}
  br i1 %c, label %H, label %X
H:
  %i = phi i32 [ 0, %G ], [ %in, %L ]
  %in = add i32 %i, 1, !revenant.va !{i64 4113}
  %c2 = icmp slt i32 %in, %n, !revenant.va !{i64 4114}
  br i1 %c2, label %L, label %X
L:
  br label %H
X:
  %r = phi i32 [ 0, %G ], [ %i, %H ]
  ret i32 %r
}

; Not a rotation: the loop's test compares a different bound/operand than
; the guard (zlib `multiply_by_power_of_ten` shaped this way and used to
; fold + crash). G must survive.
; CHECK-LABEL: define i32 @keep(
; CHECK: {{^}}G:
; CHECK: br i1 %c414, label %H, label %X
define i32 @keep(i32 %n, i32 %m) {
Entry:
  br label %G
G:
  %c414 = icmp ult i32 %n, 115, !revenant.va !{i64 4096}
  br i1 %c414, label %H, label %X
H:
  %i = phi i32 [ 0, %G ], [ %in, %L ]
  %in = add i32 %i, 1, !revenant.va !{i64 4113}
  %c633 = icmp ult i32 %in, %m, !revenant.va !{i64 4114}
  br i1 %c633, label %L, label %X
L:
  br label %H
X:
  %r = phi i32 [ 7, %G ], [ %in, %H ]
  ret i32 %r
}

; Non-branch Entry: a switch predecessor can never be retargeted by the
; fold. The pass must decline (previously it dereferenced a null
; BranchInst).
; CHECK-LABEL: define i32 @keep_switch(
; CHECK: {{^}}G:
; CHECK: br i1 %c, label %H, label %X
define i32 @keep_switch(i32 %n) {
Entry:
  switch i32 %n, label %Out [
    i32 1, label %G
    i32 2, label %G
  ]
G:
  %c = icmp sgt i32 %n, 0, !revenant.va !{i64 4096}
  br i1 %c, label %H, label %X
H:
  %i = phi i32 [ 0, %G ], [ %in, %L ]
  %in = add i32 %i, 1, !revenant.va !{i64 4113}
  %c2 = icmp slt i32 %in, %n, !revenant.va !{i64 4114}
  br i1 %c2, label %L, label %X
L:
  br label %H
X:
  %r = phi i32 [ 7, %G ], [ %in, %H ]
  ret i32 %r
Out:
  ret i32 -1
}
