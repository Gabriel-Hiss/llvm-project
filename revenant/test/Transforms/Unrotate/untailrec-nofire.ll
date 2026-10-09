; RUN: opt -load-pass-plugin=%shlibdir/RevenantTransformUnrotate.dll -passes=revenant-untailrec -S %s | FileCheck %s

; Pre-opt recursive shape (from c-clang-o2 main.pre.ll @tail_gcd): the
; function already calls itself, so the pass must leave it untouched.
; CHECK-LABEL: define i32 @tail_gcd_pre(
; CHECK: call i32 @tail_gcd_pre(
; CHECK: ret i32
define i32 @tail_gcd_pre(i32 %a, i32 %b) {
entry:
  %c = icmp eq i32 %b, 0
  br i1 %c, label %ret_a, label %rec
ret_a:
  ret i32 %a
rec:
  %r = srem i32 %a, %b
  %t = call i32 @tail_gcd_pre(i32 %b, i32 %r), !revenant.va !{i64 8192}
  ret i32 %t
}
