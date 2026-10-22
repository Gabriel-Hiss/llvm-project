; RUN: %revenant-opt -passes=revenant-unswitch -S %s | %FileCheck %s
;
; Linear map whose affine function runs on the switch value itself
; instead of the range-checked offset: clang emits `mul %c, 10` for
; `case 3..12: return x*10`, not `mul (x-3), 10`.
; Corpus: c-clang-o2 / c-clang-o2avx2 @switch_offset 0x140001250
; (cases 3..12 -> 30..120, default -1).

define i32 @sub_140001250(i32 %0) {
bb_140001250:
  %narrow = add i32 %0, -3
  %narrow39 = mul i32 %0, 10
  %c = icmp ult i32 %narrow, 10
  %2 = select i1 %c, i32 %narrow39, i32 -1
  ret i32 %2
}

; CHECK-LABEL: define i32 @sub_140001250(
; CHECK:       switch i32 %0, label %sw.tail [
; CHECK-DAG:     i32 3, label %sw.case
; CHECK-DAG:     i32 12, label %sw.case9
; CHECK:       ]
; CHECK:       %sw.ret = phi i32 [ -1, %bb_140001250 ]
; CHECK-DAG:     [ 30, %sw.case
; CHECK-DAG:     [ 120, %sw.case9
; CHECK:       ret i32 %sw.ret

; The offset form still recovers when the map scales the offset.
define i32 @off(i32 %0) {
bb_0:
  %sub = add i32 %0, -7
  %scaled = mul i32 %sub, 10
  %res = add i32 %scaled, 1000
  %c = icmp ult i32 %sub, 8
  %2 = select i1 %c, i32 %res, i32 -1
  ret i32 %2
}

; CHECK-LABEL: define i32 @off(
; CHECK:       switch i32 %0, label %sw.tail [
; CHECK-DAG:     i32 7, label %sw.case
; CHECK-DAG:     i32 14, label %sw.case7
; CHECK:       ]
; CHECK-DAG:     [ 1000, %sw.case
; CHECK-DAG:     [ 1070, %sw.case7
