; RUN: %revenant-opt -passes=revenant-idioms -S %s -o - | %FileCheck %s
; GCC O2 magic shapes, transcribed from real `gcc -O2 -S` output
; (divbattery.c, MinGW 13.2). GCC corrects with the arithmetic sign
; (`sar x,31`, i.e. 0/-1) subtracted at the end, and negates the whole
; positive DAG for negative divisors instead of changing the magic.
; gcc sdiv3: movsx; sar sign; imul 1431655766; shr32; sub sign
define i32 @gcc_sdiv3(i32 %x) {
entry:
  %e = sext i32 %x to i64
  %sg = ashr i32 %x, 31
  %m = mul i64 %e, 1431655766
  %h = lshr i64 %m, 32
  %t = trunc i64 %h to i32
  %q = sub i32 %t, %sg
  ret i32 %q
}
; CHECK-LABEL: define i32 @gcc_sdiv3(
; CHECK: sdiv i32 %x, 3

; gcc sdiv_neg3: negation of the sdiv3 dag, same magic 1431655766
define i32 @gcc_sdiv_neg3(i32 %x) {
entry:
  %e = sext i32 %x to i64
  %sg = ashr i32 %x, 31
  %m = mul i64 %e, 1431655766
  %h = lshr i64 %m, 32
  %t = trunc i64 %h to i32
  %s = sub i32 %sg, %t
  ret i32 %s
}
; CHECK-LABEL: define i32 @gcc_sdiv_neg3(
; CHECK: sdiv i32 %x, -3

; gcc sdiv5: movsx; sar sign; imul 1717986919; sar33; sub sign
define i32 @gcc_sdiv5(i32 %x) {
entry:
  %e = sext i32 %x to i64
  %sg = ashr i32 %x, 31
  %m = mul i64 %e, 1717986919
  %h = ashr i64 %m, 33
  %t = trunc i64 %h to i32
  %q = sub i32 %t, %sg
  ret i32 %q
}
; CHECK-LABEL: define i32 @gcc_sdiv5(
; CHECK: sdiv i32 %x, 5

; gcc sdiv_neg5: negation of the sdiv5 dag
define i32 @gcc_sdiv_neg5(i32 %x) {
entry:
  %e = sext i32 %x to i64
  %sg = ashr i32 %x, 31
  %m = mul i64 %e, 1717986919
  %h = ashr i64 %m, 33
  %t = trunc i64 %h to i32
  %s = sub i32 %sg, %t
  ret i32 %s
}
; CHECK-LABEL: define i32 @gcc_sdiv_neg5(
; CHECK: sdiv i32 %x, -5

; gcc sdiv7: imul -1840700269; shr32; add x; sar31 sign; sar2; sub sign
define i32 @gcc_sdiv7(i32 %x) {
entry:
  %e = sext i32 %x to i64
  %m = mul i64 %e, -1840700269
  %h = lshr i64 %m, 32
  %t = trunc i64 %h to i32
  %s = add i32 %t, %x
  %sg = ashr i32 %x, 31
  %a = ashr i32 %s, 2
  %q = sub i32 %a, %sg
  ret i32 %q
}
; CHECK-LABEL: define i32 @gcc_sdiv7(
; CHECK: sdiv i32 %x, 7

; gcc sdiv_neg7: negation of the sdiv7 dag
define i32 @gcc_sdiv_neg7(i32 %x) {
entry:
  %e = sext i32 %x to i64
  %m = mul i64 %e, -1840700269
  %h = lshr i64 %m, 32
  %t = trunc i64 %h to i32
  %s = add i32 %t, %x
  %sg = ashr i32 %x, 31
  %a = ashr i32 %s, 2
  %n = sub i32 %sg, %a
  ret i32 %n
}
; CHECK-LABEL: define i32 @gcc_sdiv_neg7(
; CHECK: sdiv i32 %x, -7

; gcc udiv3: mov edx,imm; single-operand imul rdx; shr33
define i32 @gcc_udiv3(i32 %x) {
entry:
  %e = zext i32 %x to i64
  %m = mul i64 %e, 2863311531
  %h = lshr i64 %m, 33
  %t = trunc i64 %h to i32
  ret i32 %t
}
; CHECK-LABEL: define i32 @gcc_udiv3(
; CHECK: udiv i32 %x, 3
