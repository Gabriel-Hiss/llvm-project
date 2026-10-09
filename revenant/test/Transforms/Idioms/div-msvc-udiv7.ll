; RUN: %revenant-opt -passes=revenant-idioms -S %s -o - | %FileCheck %s
; div7: MSVC udiv7 shape with (x-hi)/2+hi round-up fixup -> udiv 7.
define i32 @div7(i32 %x) {
entry:
  %e = zext i32 %x to i64
  %m = mul i64 %e, 613566757
  %hs = lshr i64 %m, 32
  %hi = trunc i64 %hs to i32
  %sub = sub i32 %x, %hi
  %s = lshr i32 %sub, 1
  %a = add i32 %s, %hi
  %q = lshr i32 %a, 2
  ret i32 %q
}
; CHECK-LABEL: define i32 @div7(
; CHECK: udiv i32 %x, 7
; CHECK-NOT: 613566757
