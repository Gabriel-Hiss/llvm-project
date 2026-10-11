; RUN: revenant-clang %s -o %t.c --lang c
; RUN: clang -I. -DTEST_SHIFT_VALUES -include %t.c -o %t.exe
; RUN: %t.exe
; REQUIRES: x86-64-target-arch

; Test that the i128 shift helpers produce correct values at counts
; 0, 1, 63, 64, 96, 127 — especially the n>=64 branch which was
; previously a copy of the shl branch (wrong for shr).

target triple = "x86_64-pc-windows-msvc"

define i128 @test_shr(i128 %a, i32 %n) {
entry:
  %result = lshr i128 %a, %n
  ret i128 %result
}

define i128 @test_shl(i128 %a, i32 %n) {
entry:
  %result = shl i128 %a, %n
  ret i128 %result
}

define i128 @test_ashr(i128 %a, i32 %n) {
entry:
  %result = ashr i128 %a, %n
  ret i128 %result
}
