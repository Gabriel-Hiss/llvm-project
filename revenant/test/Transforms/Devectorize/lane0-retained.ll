; Negative: a returned <2 x double> keeps full width. The extract stays a
; vector extract; nothing scalarizes. Ports lane0_returned_i128_stays.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-devectorize -S %s | %FileCheck %s

define <2 x double> @lane0_retained(ptr %p0, ptr %q) {
entry:
  %x = load <2 x double>, ptr %p0, align 16
  %e = extractelement <2 x double> %x, i32 0
  store double %e, ptr %q, align 8
  ret <2 x double> %x
}

; CHECK: %x = load <2 x double>, ptr %p0
; CHECK: extractelement <2 x double> %x, i32 0
; CHECK: ret <2 x double> %x
