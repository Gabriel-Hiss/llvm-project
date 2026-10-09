; RUN: %revenant-opt -passes=revenant-idioms -S %s -o - | %FileCheck %s
; Plain C shift/or byte shuffles remain source-level arithmetic operations.
define i32 @bswap32(i32 %x) {
entry:
  %a = lshr i32 %x, 24
  %b0 = lshr i32 %x, 8
  %b = and i32 %b0, 65280
  %c0 = shl i32 %x, 8
  %c = and i32 %c0, 16711680
  %d = shl i32 %x, 24
  %o1 = or i32 %a, %b
  %o2 = or i32 %o1, %c
  %o3 = or i32 %o2, %d
  ret i32 %o3
}
; CHECK-LABEL: define i32 @bswap32(
; CHECK: lshr i32 %x, 24
; CHECK: shl i32 %x, 24
; CHECK: or i32 %o2, %d

; A shift/or is not enough to prove a byte swap.
define i16 @not_bswap(i16 %x) {
entry:
  %left = shl i16 %x, 1
  %right = lshr i16 %x, 1
  %combined = or i16 %left, %right
  ret i16 %combined
}
; CHECK-LABEL: define i16 @not_bswap(
; CHECK: shl i16 %x, 1
; CHECK: lshr i16 %x, 1
; CHECK-NOT: call i16 @llvm.bswap.i16

define i32 @bswap_builtin(i32 %x) {
entry:
  %result = call i32 @llvm.bswap.i32(i32 %x)
  ret i32 %result
}
; CHECK-LABEL: define i32 @bswap_builtin(
; CHECK: call i32 @llvm.bswap.i32(i32 %x)
