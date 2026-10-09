; The phi web s<->s2 and i<->i2 coalesces into two locals; the loop
; backedge copies land on the latch edge, not as self-assigns.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-ssaout -S %s | %FileCheck %s

; CHECK-NOT: phi i32
; CHECK: alloca i32
; CHECK-NOT: phi i32
; CHECK: store i32
; CHECK-NOT: phi i32
; CHECK: ret i32
; CHECK-NOT: phi i32

define i32 @sum(i32 %p0) {
entry:
  br label %loop, !revenant.va !0

loop:
  %s = phi i32 [ 0, %entry ], [ %s2, %body ], !revenant.va !1
  %i = phi i32 [ 0, %entry ], [ %i2, %body ], !revenant.va !1
  %c = icmp slt i32 %i, %p0, !revenant.va !1
  br i1 %c, label %body, label %exit, !revenant.va !1

body:
  %s2 = add i32 %s, %i, !revenant.va !2
  %i2 = add i32 %i, 1, !revenant.va !2
  br label %loop, !revenant.va !2

exit:
  ret i32 %s, !revenant.va !3
}

!0 = !{i64 4198496}
!1 = !{i64 4198499}
!2 = !{i64 4198505}
!3 = !{i64 4198510}
