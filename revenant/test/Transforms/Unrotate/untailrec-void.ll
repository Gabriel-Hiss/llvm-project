; RUN: opt -load-pass-plugin=%shlibdir/RevenantTransformUnrotate.dll -passes=revenant-untailrec -S %s | FileCheck %s

; Void loop with per-argument header phis becomes a recursive void call.
; The latch computes both next values itself.
define void @count_dn(i32 %n, i32 %acc) {
; CHECK-LABEL: define void @count_dn(
entry:
  %c = icmp sle i32 %n, 0
  br i1 %c, label %exit, label %header
header:
  %pn = phi i32 [%n, %entry], [%dec, %latch]
  %pa = phi i32 [%acc, %entry], [%sum, %latch]
  %done = icmp sle i32 %pn, 0
  br i1 %done, label %exit, label %latch
latch:
; CHECK: latch:
; CHECK: call void @count_dn(i32 %dec, i32 %sum)
; CHECK: ret void
  %dec = sub i32 %pn, 1
  %sum = add i32 %pa, %pn
  br label %header
exit:
  ret void
}
