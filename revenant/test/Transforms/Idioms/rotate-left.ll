; RUN: revenant-idioms-test %s -o - | FileCheck %s
; rotate-left: lifter rol shape (x<<c)|(x>>(32-c)) -> llvm.fshl.
define i32 @rotl32(i32 %x, i32 %c) {
entry:
  %w = sub i32 32, %c
  %l = shl i32 %x, %c
  %r = lshr i32 %x, %w
  %o = or i32 %l, %r
  ret i32 %o
}
; CHECK-LABEL: define i32 @rotl32(
; CHECK: call i32 @llvm.fshl.i32(i32 %x, i32 %x, i32 %c)
