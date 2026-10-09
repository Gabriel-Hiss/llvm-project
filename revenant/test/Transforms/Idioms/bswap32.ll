; RUN: revenant-idioms-test %s -o - | FileCheck %s
; bswap32: shift-and-or byte shuffle -> llvm.bswap.
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
; CHECK: call i32 @llvm.bswap.i32(i32 %x)
