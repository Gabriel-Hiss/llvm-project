; Integer narrowing: the lifter widens every int register to i64 (sigs
; over-approximates because one 64-bit read widens the whole parameter).
; The annotation restores the ir-pre widths. Corpus: c-clang-o2
; @arith(i32, i32); lifted shape is (i64, i64) -> i64 after cleanup.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unabi -S %s | %FileCheck %s

; CHECK-LABEL: define i32 @arith(i32 %a0, i32 %a1)
; CHECK: ret i32
define i64 @arith(i64 %0, i64 %1) #0 {
entry:
  %t0 = trunc i64 %0 to i32, !revenant.va !{i64 4096}
  %t1 = trunc i64 %1 to i32, !revenant.va !{i64 4096}
  %m = mul i32 %t0, %t1, !revenant.va !{i64 4100}
  %e = zext i32 %m to i64, !revenant.va !{i64 4104}
  ret i64 %e
}

attributes #0 = { "revenant.sig"="r32 r32 => r32" }
