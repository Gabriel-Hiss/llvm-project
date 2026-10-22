; i128 overflow-check guard: the 2^64 bound needs 65 bits, so the
; packed-reduction matcher must bail before converting it.
; RUN: %revenant-opt -passes=revenant-unloopvec,verify -S %s | %FileCheck %s

define i64 @checked_mul(ptr %0, i64 %1) {
bb_140006a50:
  %3 = load i64, ptr %0, align 1
  %4 = sext i64 %1 to i128
  %5 = sext i64 %3 to i128
  %6 = mul nsw i128 %5, %4
  %7 = add i128 %6, 9223372036854775808
  %.not = icmp ult i128 %7, 18446744073709551616
  br i1 %.not, label %ok, label %trap

ok:
  %8 = trunc i128 %6 to i64
  ret i64 %8

trap:
  ret i64 0
}

; CHECK-LABEL: define{{.*}}i64 @checked_mul
; CHECK: icmp ult i128 %6, 18446744073709551616
; CHECK: ret i64
