; RUN: revenant-idioms-test %s -o - | FileCheck %s
; Narrow-width magic-division graphs exercise the pass's exhaustive proof path.
; The divisor matches div_by_const7 in corpus src/c/main.c.
define i8 @udiv7_i8(i8 %x) {
entry:
  %wide16 = zext i8 %x to i16
  %wide32 = zext i16 %wide16 to i32
  %product = mul i32 %wide32, 293
  %quotient = lshr i32 %product, 11
  %result = trunc i32 %quotient to i8
  ret i8 %result
}
; CHECK-LABEL: define i8 @udiv7_i8(
; CHECK: udiv i8 %x, 7

define i16 @udiv7_i16(i16 %x) {
entry:
  %wide32 = zext i16 %x to i32
  %wide64 = zext i32 %wide32 to i64
  %product = mul i64 %wide64, 149797
  %quotient = lshr i64 %product, 20
  %result = trunc i64 %quotient to i16
  ret i16 %result
}
; CHECK-LABEL: define i16 @udiv7_i16(
; CHECK: udiv i16 %x, 7
