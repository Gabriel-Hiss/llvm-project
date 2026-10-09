; Independent webs on one backedge: each phi's incoming value lives on
; the latch edge, so the loop-carried values survive in distinct locals
; with stores on the backedge and loads where they are read.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-ssaout -S %s | %FileCheck %s

; CHECK-NOT: phi i32
; CHECK: alloca i32
; CHECK: store i32
; CHECK-NOT: phi i32
; CHECK: ret i32
; CHECK-NOT: phi i32

define i32 @swaploop(i32 %n) {
entry:
  br label %loop, !revenant.va !0

loop:
  %x = phi i32 [ 0, %entry ], [ %x2, %body ], !revenant.va !1
  %y = phi i32 [ 1, %entry ], [ %y2, %body ], !revenant.va !1
  %c = icmp slt i32 %x, %n, !revenant.va !1
  br i1 %c, label %body, label %exit, !revenant.va !1

body:
  %x2 = add i32 %y, 1, !revenant.va !2
  %y2 = add i32 %x, 2, !revenant.va !2
  br label %loop, !revenant.va !2

exit:
  %r = add i32 %x, %y, !revenant.va !3
  ret i32 %r, !revenant.va !3
}

!0 = !{i64 4198496}
!1 = !{i64 4198499}
!2 = !{i64 4198505}
!3 = !{i64 4198510}
