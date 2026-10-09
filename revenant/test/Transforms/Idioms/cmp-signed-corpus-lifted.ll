; RUN: revenant-opt -passes=revenant-idioms -S %s -o - | FileCheck %s
; Real c-clang-o2 lift, corpus src/c/main.c:cmp_signed, VA 0x140001070.
; The subtraction flags reduce to the source-level signed compares/selects.
define i32 @cmp_signed(i32 %a, i32 %b) {
entry:
  %difference = sub i32 %a, %b
  %unequal = icmp ne i32 %a, %b
  %xy = xor i32 %a, %b
  %xd = xor i32 %a, %difference
  %overflow = and i32 %xy, %xd
  %less = icmp slt i32 %a, %b
  %sign = sext i1 %less to i32
  %adjusted = xor i32 %difference, %overflow
  %nonnegative = icmp sgt i32 %adjusted, -1
  %positive = and i1 %nonnegative, %unequal
  %positive_i32 = zext i1 %positive to i32
  %result = add nsw i32 %sign, %positive_i32
  ret i32 %result
}
; CHECK-LABEL: define i32 @cmp_signed(
; CHECK: icmp slt i32 %a, %b
; CHECK: icmp sgt i32 %a, %b
; CHECK: select i1 {{.*}}, i32 1, i32 0
; CHECK: select i1 {{.*}}, i32 -1, i32 {{.*}}
; CHECK-NOT: call i32 @llvm.scmp

define i32 @scmp_intrinsic(i32 %a, i32 %b) {
entry:
  %result = call i32 @llvm.scmp.i32.i32(i32 %a, i32 %b)
  ret i32 %result
}
; CHECK-LABEL: define i32 @scmp_intrinsic(
; CHECK: icmp slt i32 %a, %b
; CHECK: icmp sgt i32 %a, %b
; CHECK-NOT: call i32 @llvm.scmp
