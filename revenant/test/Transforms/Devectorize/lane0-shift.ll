; Negative: `lshr <2 x i64> %x, 1` moves lane 1 into lane 0, so an
; extract of it must keep reading the full-width shift. Ports
; lane0_right_shift_stays_full_width.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-devectorize -S %s | %FileCheck %s

define i64 @lane0_shift(ptr %p0) {
entry:
  %x = load <2 x i64>, ptr %p0, align 16
  %y = lshr <2 x i64> %x, <i64 1, i64 1>
  %e = extractelement <2 x i64> %y, i32 0
  ret i64 %e
}

; CHECK: load <2 x i64>, ptr %p0
; CHECK: lshr <2 x i64> %x,
; CHECK: extractelement <2 x i64> %y, i32 0
