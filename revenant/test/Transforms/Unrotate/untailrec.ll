; RUN: opt -load-pass-plugin=%shlibdir/RevenantTransformUnrotate.dll -passes=revenant-untailrec -S %s | FileCheck %s

; Post-TailCallElim @tail_gcd loop (from c-clang-o2 main.post.ll, renamed):
; the latch computes the next values itself, so the pass rebuilds the
; recursive call the pre-opt IR has.
define i32 @tail_gcd(i32 %a, i32 %b) {
; CHECK-LABEL: define i32 @tail_gcd(
entry:
  %c = icmp eq i32 %b, 0
  br i1 %c, label %exit, label %header
header:
  %pb = phi i32 [%b, %entry], [%nb, %latch]
  %pa = phi i32 [%a, %entry], [%pb, %latch]
  %done = icmp eq i32 %pb, 0
  br i1 %done, label %exit2, label %latch
latch:
; CHECK: latch:
; CHECK: = call i32 @tail_gcd(i32 %b, i32 %nb)
; CHECK: ret i32
  %nb = srem i32 %pa, %pb, !revenant.va !{i64 8192}
  br label %header
exit:
  ret i32 %a
exit2:
  ret i32 %pb
}
