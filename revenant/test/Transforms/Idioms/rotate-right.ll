; RUN: revenant-idioms-test %s -o - | FileCheck %s
; rotate-right: ror mirror shape -> llvm.fshl (frontend prints ror).
define i32 @rotr32(i32 %x, i32 %c) {
entry:
  %w = sub i32 32, %c
  %r = lshr i32 %x, %c
  %l = shl i32 %x, %w
  %o = or i32 %r, %l
  ret i32 %o
}
; CHECK-LABEL: define i32 @rotr32(
; CHECK: call i32 @llvm.fshl.i32(i32 %x, i32 %x, i32 %w)
