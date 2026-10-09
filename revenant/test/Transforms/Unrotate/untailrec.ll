; RUN: opt -load-pass-plugin=%shlibdir/RevenantTransformUnrotate.dll -passes=revenant-untailrec -S %s | FileCheck %s

; Mirrors unopt's tail_call_to_loop: return g(p1, p0 % p1) on the tail.
define i32 @g(i32 %p0, i32 %p1) {
; CHECK-LABEL: define i32 @g(
entry:
; CHECK: entry.lp:
; CHECK: phi i32 [ %p0.in, %entry ]
  %c = icmp eq i32 %p1, 0
  br i1 %c, label %X, label %T
T:
; CHECK: T:
; CHECK: br label %entry.lp
  %r = srem i32 %p0, %p1
  %t = call i32 @g(i32 %p1, i32 %r), !revenant.va !{i64 8192}
  ret i32 %t
X:
  ret i32 %p0
; CHECK: ret i32
}
