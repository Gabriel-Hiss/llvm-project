; Straight-line SLP: a vector load feeding a lane-wise add, consumed by
; a lane-0 extract, explodes into scalar loads + a scalar add. Ports
; crates/unopt devectorize_straight_line.
; RUN: %revenant-opt -passes=revenant-devectorize -S %s | %FileCheck %s

define i32 @slp_add(ptr %p0) {
entry:
  %v = load <4 x i32>, ptr %p0, align 16
  %s = add <4 x i32> %v, %v
  %l = extractelement <4 x i32> %s, i32 0
  ret i32 %l
}

; The scalar add doubles the lane-0 load.
; CHECK: %v.l = load i32, ptr {{.*}}, align 4
; CHECK: add i32 %v.l, %v.l
; No vector ops survive.
; CHECK-NOT: <4 x i32>
; CHECK: ret i32
