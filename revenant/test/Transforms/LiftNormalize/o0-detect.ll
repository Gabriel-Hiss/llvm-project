; RUN: %revenant-opt -passes='function(revenant-lift-normalize),verify' -S %s | %FileCheck %s
; Raw lifted IR. An unoptimized function stores its register argument to
; the frame in the prologue and never computes from the register again; the
; frame is kept (volatile slots pinned for cleanup). Optimized code that
; stores an argument to the frame keeps using the register.
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"

; CHECK-LABEL: define i32 @spilled(
; CHECK: call void @revenant.o0.keep(
; CHECK: store volatile
define i32 @spilled(i64 %0) {
entry:
  %frame = alloca [16 x i8], !revenant.va !1
  %rax = alloca i64, !revenant.va !1
  %rcx = alloca i64, !revenant.va !1
  store i64 %0, ptr %rcx, !revenant.va !1
  %sp0 = getelementptr inbounds i8, ptr %frame, i64 16, !revenant.va !1
  %r = load i64, ptr %rcx, !revenant.va !2
  %slot = getelementptr inbounds i8, ptr %sp0, i64 -8, !revenant.va !2
  %t = trunc i64 %r to i32, !revenant.va !2
  store i32 %t, ptr %slot, !revenant.va !2
  %v = load i32, ptr %slot, !revenant.va !3
  %w = add i32 %v, 1, !revenant.va !3
  ret i32 %w, !revenant.va !4
}

; CHECK-LABEL: define i32 @kept_in_register(
; CHECK-NOT: revenant.o0.keep
; CHECK-NOT: volatile
; CHECK: ret i32
define i32 @kept_in_register(i64 %0) {
entry:
  %frame = alloca [16 x i8], !revenant.va !5
  %rax = alloca i64, !revenant.va !5
  %rcx = alloca i64, !revenant.va !5
  store i64 %0, ptr %rcx, !revenant.va !5
  %sp0 = getelementptr inbounds i8, ptr %frame, i64 16, !revenant.va !5
  %r = load i64, ptr %rcx, !revenant.va !6
  %slot = getelementptr inbounds i8, ptr %sp0, i64 -8, !revenant.va !6
  %t = trunc i64 %r to i32, !revenant.va !6
  store i32 %t, ptr %slot, !revenant.va !6
  %again = load i64, ptr %rcx, !revenant.va !7
  %d = add i64 %again, %again, !revenant.va !7
  %dt = trunc i64 %d to i32, !revenant.va !7
  %v = load i32, ptr %slot, !revenant.va !8
  %w = add i32 %v, %dt, !revenant.va !8
  ret i32 %w, !revenant.va !9
}

; CHECK: attributes #{{[0-9]+}} = { "revenant.o0" }

!1 = !{i64 5368713216}
!2 = !{i64 5368713220}
!3 = !{i64 5368713224}
!4 = !{i64 5368713228}
!5 = !{i64 5368713472}
!6 = !{i64 5368713476}
!7 = !{i64 5368713480}
!8 = !{i64 5368713484}
!9 = !{i64 5368713488}
