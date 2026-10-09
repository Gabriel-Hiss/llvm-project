; RUN: revenant-opt -passes=revenant-idioms -S %s -o - | FileCheck %s
; Source-level rotate shift/or shapes stay as plain arithmetic operations.
define i32 @rotl32(i32 %x, i32 %c) {
entry:
  %w = sub i32 32, %c
  %l = shl i32 %x, %c
  %r = lshr i32 %x, %w
  %o = or i32 %l, %r
  ret i32 %o
}
; CHECK-LABEL: define i32 @rotl32(
; CHECK: shl i32 %x, %c
; CHECK: lshr i32 %x, %w
; CHECK: or i32 %l, %r
