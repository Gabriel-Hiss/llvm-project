; A vector load feeding an add stays exploded across a clobbering call:
; the scalar lanes materialize at the load (before the call), so the
; read is not moved past the write. Ports
; straight_load_stays_before_clobber_call.
; RUN: %revenant-opt -passes=revenant-devectorize -S %s | %FileCheck %s

declare void @clobber(ptr)

define i32 @slp_clobber(ptr %p) {
entry:
  %x = load <4 x i32>, ptr %p, align 16
  %s = add <4 x i32> %x, %x
  call void @clobber(ptr %p)
  %e = extractelement <4 x i32> %s, i32 0
  ret i32 %e
}

; Scalar lanes materialize before the call, the add folds per lane.
; CHECK: load i32, ptr {{.*}}, align 4
; CHECK: call void @clobber
; The extract resolves to the lane-0 scalar add.
; CHECK: ret i32
