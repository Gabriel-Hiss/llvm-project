; Scalar i128 load/phi/store web splits into i64 halves (zlib-cl-o2
; sub_140050B44 shape): one arm loads 16 bytes, the other is a folded
; constant; the phi feeds a single 16-byte store. All three become i64
; pairs with the same bytes.
; RUN: %revenant-opt -passes=revenant-devectorize -S %s | %FileCheck %s

define void @pair_phi(ptr %src, ptr %dst, i1 %c) {
entry:
  br i1 %c, label %load.arm, label %const.arm

load.arm:
  %v = load i128, ptr %src, align 1
  br label %join

const.arm:
  br label %join

join:
  %p = phi i128 [ %v, %load.arm ], [ 99046709483913839067215266512, %const.arm ]
  store i128 %p, ptr %dst, align 1
  ret void
}

; CHECK-NOT: i128
; The load becomes two i64 loads, the phi two i64 phis, the store two stores.
; CHECK: load i64
; CHECK: phi i64
; CHECK-COUNT-2: store i64
; CHECK: ret void
