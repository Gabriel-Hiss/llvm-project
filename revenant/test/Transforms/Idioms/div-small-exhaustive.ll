; RUN: revenant-idioms-test %s -o - | FileCheck %s
; Narrow-width magic-division graphs exercise the pass's exhaustive proof path.
; The divisor matches div_by_const7 in corpus src/c/main.c.
define i8 @udiv7_i8(i8 %x) {
entry:
  %wide = zext i8 %x to i16
  %product = mul i16 %wide, 293
  %quotient = lshr i16 %product, 11
  %result = trunc i16 %quotient to i8
  ret i8 %result
}
; CHECK-LABEL: define i8 @udiv7_i8(
; CHECK: udiv i8 %x, 7

define i16 @udiv7_i16(i16 %x) {
entry:
  %wide = zext i16 %x to i32
  %product = mul i32 %wide, 9363
  %quotient = lshr i32 %product, 16
  %result = trunc i32 %quotient to i16
  ret i16 %result
}
; CHECK-LABEL: define i16 @udiv7_i16(
; CHECK: udiv i16 %x, 7
