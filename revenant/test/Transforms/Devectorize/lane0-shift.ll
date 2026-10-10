; Per-lane `lshr <2 x i64> %x, <1, 1>` shifts each lane in place (LLVM vector
; shifts never move bits across lanes), so extracting lane 0 after the shift
; is just lane 0 shifted: the straight-line phase explodes it into scalar
; loads + a scalar shift. (The old "stays full width" premise came from the
; plain-Rust reference where an lshr on a 128-bit pseudo-vector does shift
; across the whole width; that premise did not survive the LLVM port.)
; RUN: %revenant-opt -passes=revenant-devectorize -S %s | %FileCheck %s

define i64 @lane0_shift(ptr %p0) {
entry:
  %x = load <2 x i64>, ptr %p0, align 16
  %y = lshr <2 x i64> %x, <i64 1, i64 1>
  %e = extractelement <2 x i64> %y, i32 0
  ret i64 %e
}

; CHECK-NOT: <2 x i64>
; CHECK: lshr i64
; CHECK: ret i64
