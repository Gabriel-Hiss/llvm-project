; RUN: %revenant-opt -passes=revenant-idioms -S %s -o - | %FileCheck %s
; Clang O2 magic shapes, transcribed from real `clang -O2 -S` output
; (divbattery.c). Signed/unsigned and positive/negative divisors must
; each recover with their own sign.
; sdiv3: movsxd; imul 1431655766; shr63+shr32; add
define i32 @clang_sdiv3(i32 %x) {
entry:
  %e = sext i32 %x to i64
  %m = mul i64 %e, 1431655766
  %s = lshr i64 %m, 63
  %h = lshr i64 %m, 32
  %a = trunc i64 %h to i32
  %b = trunc i64 %s to i32
  %q = add i32 %a, %b
  ret i32 %q
}
; CHECK-LABEL: define i32 @clang_sdiv3(
; CHECK: sdiv i32 %x, 3

; sdiv_neg3: imul 1431655765 (one less); shr32; sub x; shr31/sar1; add
define i32 @clang_sdiv_neg3(i32 %x) {
entry:
  %e = sext i32 %x to i64
  %m = mul i64 %e, 1431655765
  %h = lshr i64 %m, 32
  %t = trunc i64 %h to i32
  %s = sub i32 %t, %x
  %c = lshr i32 %s, 31
  %a = ashr i32 %s, 1
  %q = add i32 %a, %c
  ret i32 %q
}
; CHECK-LABEL: define i32 @clang_sdiv_neg3(
; CHECK: sdiv i32 %x, -3

; sdiv5: imul 1717986919; shr63/sar33; add
define i32 @clang_sdiv5(i32 %x) {
entry:
  %e = sext i32 %x to i64
  %m = mul i64 %e, 1717986919
  %s = lshr i64 %m, 63
  %h = ashr i64 %m, 33
  %a = trunc i64 %h to i32
  %b = trunc i64 %s to i32
  %q = add i32 %a, %b
  ret i32 %q
}
; CHECK-LABEL: define i32 @clang_sdiv5(
; CHECK: sdiv i32 %x, 5

; sdiv7: imul -1840700269; shr32; add x; shr31/sar2; add
define i32 @clang_sdiv7(i32 %x) {
entry:
  %e = sext i32 %x to i64
  %m = mul i64 %e, -1840700269
  %h = lshr i64 %m, 32
  %t = trunc i64 %h to i32
  %s = add i32 %x, %t
  %c = lshr i32 %s, 31
  %a = ashr i32 %s, 2
  %q = add i32 %a, %c
  ret i32 %q
}
; CHECK-LABEL: define i32 @clang_sdiv7(
; CHECK: sdiv i32 %x, 7

; sdiv_neg7: imul 1840700269; shr32; sub x; shr31/sar2; add
define i32 @clang_sdiv_neg7(i32 %x) {
entry:
  %e = sext i32 %x to i64
  %m = mul i64 %e, 1840700269
  %h = lshr i64 %m, 32
  %t = trunc i64 %h to i32
  %s = sub i32 %t, %x
  %c = lshr i32 %s, 31
  %a = ashr i32 %s, 2
  %q = add i32 %a, %c
  ret i32 %q
}
; CHECK-LABEL: define i32 @clang_sdiv_neg7(
; CHECK: sdiv i32 %x, -7

; udiv5: mov+mov eax,imm; imul rax,rcx; shr34
define i32 @clang_udiv5(i32 %x) {
entry:
  %e = zext i32 %x to i64
  %m = mul i64 %e, 3435973837
  %h = lshr i64 %m, 34
  %t = trunc i64 %h to i32
  ret i32 %t
}
; CHECK-LABEL: define i32 @clang_udiv5(
; CHECK: udiv i32 %x, 5

; udiv10: same with shr35
define i32 @clang_udiv10(i32 %x) {
entry:
  %e = zext i32 %x to i64
  %m = mul i64 %e, 3435973837
  %h = lshr i64 %m, 35
  %t = trunc i64 %h to i32
  ret i32 %t
}
; CHECK-LABEL: define i32 @clang_udiv10(
; CHECK: udiv i32 %x, 10
