; RUN: opt -load-pass-plugin=%shlibdir/RevenantTransformUnrotate.dll -passes=revenant-untailrec -S %s | FileCheck %s

; Non-tail self-call (result unused): must stay recursive.
define i32 @h(i32 %p0) {
; CHECK-LABEL: define i32 @h(
; CHECK: call i32 @h(i32 %{{.*}})
; CHECK: ret i32 %p0
entry:
  %c = icmp eq i32 %p0, 0
  br i1 %c, label %X, label %T
T:
  %t = call i32 @h(i32 %p0), !revenant.va !{i64 8192}
  br label %X
X:
  %r = phi i32 [0, %entry], [%t, %T]
  ret i32 %p0
}
