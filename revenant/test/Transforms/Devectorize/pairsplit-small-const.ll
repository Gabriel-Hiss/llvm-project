; Negative: small i128 constants stay whole. A constant that fits in 64
; bits materializes through the u64-literal path, so splitting only
; lengthens the source.
; RUN: %revenant-opt -passes=revenant-devectorize -S %s | %FileCheck %s

define void @pair_small_const(ptr %p) {
entry:
  store i128 4602678819172646912, ptr %p, align 1
  ret void
}

; CHECK: store i128 4602678819172646912
; CHECK: ret void
