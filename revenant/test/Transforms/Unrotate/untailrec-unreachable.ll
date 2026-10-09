; RUN: opt -load-pass-plugin=%shlibdir/RevenantTransformUnrotate.dll -passes=revenant-untailrec -S %s | FileCheck %s

; Latch with a store (side effect): not the TailCallElim shape, untouched.
; The exit returns a function argument (dominates everything), and the
; loop-carried values stay inside the loop, so the input is valid.
; CHECK-LABEL: define i32 @impure(
; CHECK: br label %header
; CHECK: ret i32 %acc
define i32 @impure(i32 %n, i32 %acc, ptr %slot) {
entry:
  %c = icmp sle i32 %n, 0
  br i1 %c, label %exit, label %header
header:
  %pn = phi i32 [%n, %entry], [%nxt, %latch]
  %pa = phi i32 [%acc, %entry], [%sum, %latch]
  br label %latch
latch:
  %sum = add i32 %pa, %pn
  %nxt = sub i32 %pn, 1
  store i32 %sum, ptr %slot
  br label %header
exit:
  ret i32 %acc
}
