; RUN: revenant-opt -passes=revenant-idioms -S %s -o - | FileCheck %s
; minmax: source-level compare/select forms stay visible; no optimizer intrinsics.
define i32 @minmax(i32 %a, i32 %b) {
entry:
  %c1 = icmp slt i32 %a, %b
  %lo = select i1 %c1, i32 %a, i32 %b
  %c2 = icmp sgt i32 %a, %b
  %hi = select i1 %c2, i32 %a, i32 %b
  %c3 = icmp slt i32 %a, 0
  %n = sub i32 0, %a
  %mag = select i1 %c3, i32 %n, i32 %a
  %s1 = add i32 %lo, %hi
  %s2 = add i32 %s1, %mag
  ret i32 %s2
}
; CHECK-LABEL: define i32 @minmax(
; CHECK: icmp slt i32 %a, %b
; CHECK: select i1 %c1, i32 %a, i32 %b
; CHECK: icmp sgt i32 %a, %b
; CHECK: select i1 %c2, i32 %a, i32 %b
; CHECK: select i1 %c3, i32 %n, i32 %a
; CHECK-NOT: call i32 @llvm.
