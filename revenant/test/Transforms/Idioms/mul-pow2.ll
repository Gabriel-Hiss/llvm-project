; RUN: revenant-idioms-test %s -o - | FileCheck %s
; mul-pow2: mul by 8 -> shl 3. odd-constant mul stays mul.
define i32 @mulpow2(i32 %x) {
entry:
  %m = mul i32 %x, 8
  ret i32 %m
}
; CHECK-LABEL: define i32 @mulpow2(
; CHECK: shl i32 %x, 3
