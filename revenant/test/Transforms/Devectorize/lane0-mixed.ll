; Negative: mixed i32/i64 extracts of one value disagree on the lane
; layout; nothing scalarizes. Ports lane0_mixed_layouts_untouched.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-devectorize -S %s | %FileCheck %s

define i64 @lane0_mixed(ptr %p0, ptr %q) {
entry:
  %x = load <4 x i32>, ptr %p0, align 16
  %e1 = extractelement <4 x i32> %x, i32 0
  %e2 = extractelement <4 x i32> %x, i32 1
  store i32 %e1, ptr %q, align 4
  %e64 = bitcast <4 x i32> %x to <2 x i64>
  %e3 = extractelement <2 x i64> %e64, i32 0
  ret i64 %e3
}

; CHECK: load <4 x i32>, ptr %p0
; CHECK: extractelement <4 x i32> %x, i32 0
; CHECK: extractelement <2 x i64> %e64, i32 0
