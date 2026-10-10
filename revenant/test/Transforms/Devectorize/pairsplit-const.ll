; Scalar i128 constant store splits into two i64 stores (zlib-cl-o2 shape):
; a folded 16-byte constant (two pointers) is stored as one i128. The split
; keeps the same bytes with scalar statements and no i128 constant for the
; C emitter to materialize.
; RUN: %revenant-opt -passes=revenant-devectorize -S %s | %FileCheck %s

define void @pair_const(ptr %p) {
entry:
  store i128 99046709483913839067215266512, ptr %p, align 1
  ret void
}

; CHECK-NOT: store i128
; CHECK-NOT: i128 99046709483913839067215266512
; Low half (0x1400982d0) lands at offset 0, high half (0x140098490) at +8.
; CHECK: store i64 5369332432, ptr {{.*}}, align 1
; CHECK: store i64 5369332880, ptr {{.*}}, align 1
; CHECK: ret void
