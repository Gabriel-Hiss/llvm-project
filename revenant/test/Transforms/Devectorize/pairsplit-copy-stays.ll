; Negative: load-only i128 copy webs stay whole. A 16-byte load/store pair
; emits cleanly as one struct assignment, which reads better and stays
; recognizable for loop folding; splitting only lengthens the source.
; RUN: %revenant-opt -passes=revenant-devectorize -S %s | %FileCheck %s

define void @pair_copy_stays(ptr %src, ptr %dst) {
entry:
  %v = load i128, ptr %src, align 1
  store i128 %v, ptr %dst, align 1
  ret void
}

; CHECK: load i128
; CHECK: store i128
; CHECK: ret void
