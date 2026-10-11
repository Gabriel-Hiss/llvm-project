; Lane-0 broadcast (zlib gz_skip shape): a scalar is inserted into lane 0 of
; a poison vector, then a zeroinitializer shuffle duplicates lane 0 to every
; lane (a true broadcast: both lanes add the scalar). The lane-wise add and
; store split into per-lane scalar ops. Source adds the same value to two
; struct fields.
; Real lifted IR: deopt-board/work/rec-zlib-cl-o2.ll.ir-clean.ll#sub_1400068B0
; (same shape in sub_140005DF0; sub_140006C30 uses zext-to-i128 + bitcast
; instead of insertelement for the broadcast).
; RUN: %revenant-opt -passes=revenant-devectorize -S %s | %FileCheck %s

define void @lane0_broadcast(ptr %p, i64 %s) {
entry:
  %v = load <2 x i64>, ptr %p, align 1
  %ins = insertelement <2 x i64> poison, i64 %s, i64 0
  %b = shufflevector <2 x i64> %ins, <2 x i64> poison, <2 x i32> zeroinitializer
  %w = add <2 x i64> %v, %b
  store <2 x i64> %w, ptr %p, align 1
  ret void
}

; Both lanes become scalar adds fed by scalar loads; no vector types survive.
; CHECK-NOT: <2 x i64>
; CHECK-NOT: insertelement
; CHECK-NOT: shufflevector
; CHECK-COUNT-2: add i64
; CHECK-COUNT-2: store i64
; CHECK: ret void
