; RUN: opt -load-pass-plugin=%shlibdir/RevenantTransformUnrotate.dll -passes=revenant-untailrec -S %s | FileCheck %s

; Void self tail-call before `ret void` becomes a backedge.
define void @v(i32 %p0) {
; CHECK-LABEL: define void @v(
entry:
; CHECK: entry.lp:
; CHECK: phi i32 [ %p0.in, %entry ]
  %c = icmp eq i32 %p0, 0
  br i1 %c, label %X, label %T
T:
; CHECK: T:
; CHECK: br label %entry.lp
  %n = sub i32 %p0, 1
  call void @v(i32 %n), !revenant.va !{i64 8192}
  ret void
X:
  ret void
; CHECK: ret void
}
