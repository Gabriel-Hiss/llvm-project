; Lane-0 broadcast via zext + bitcast (zlib gzgets shape): the scalar is
; zero-extended to i128 (the low lane), bitcast to <2 x i64>, then a
; zeroinitializer shuffle duplicates lane 0 to every lane: both lanes add
; the scalar. Same scalarization as the insertelement form.
; Real lifted IR: deopt-board/work/rec-zlib-cl-o2.ll.ir-clean.ll#sub_140006C30.
; RUN: %revenant-opt -passes=revenant-devectorize -S %s | %FileCheck %s

define void @lane0_broadcast_zext(ptr %p, i32 %s) {
entry:
  %v = load <2 x i64>, ptr %p, align 1
  %z = zext i32 %s to i128
  %b0 = bitcast i128 %z to <2 x i64>
  %b = shufflevector <2 x i64> %b0, <2 x i64> poison, <2 x i32> zeroinitializer
  %w = add <2 x i64> %v, %b
  store <2 x i64> %w, ptr %p, align 1
  ret void
}

; CHECK-NOT: <2 x i64>
; CHECK-NOT: shufflevector
; CHECK-NOT: i128
; CHECK-COUNT-2: add i64
; CHECK-COUNT-2: store i64
; CHECK: ret void
