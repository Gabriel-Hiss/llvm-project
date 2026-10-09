; RUN: opt -load-pass-plugin=%shlibdir/RevenantTransformUnrotate.dll -passes=revenant-untailrec -S %s | FileCheck %s

; Self-call before `unreachable` (the shape MSVC emits at O0) is a tail
; call whose result would be returned.
define i32 @u(i32 %p0, i32 %p1) {
; CHECK-LABEL: define i32 @u(
entry:
; CHECK: entry.lp:
; CHECK: phi i32 [ %p0.in, %entry ]
  %c = icmp eq i32 %p1, 0
  br i1 %c, label %X, label %T
T:
; CHECK: T:
; CHECK: br label %entry.lp
  %r = srem i32 %p0, %p1
  call i32 @u(i32 %p1, i32 %r), !revenant.va !{i64 8192}
  unreachable
X:
  ret i32 %p0
; CHECK: ret i32
}
