; Saturating subtract lane web (zlib-clang-o2 shape): a loaded <8 x i16>
; pair is compared lane-wise, subtracted lane-wise, and the comparison mask
; selects between zero and the difference (unsigned saturation). All three
; vector ops explode into per-lane scalar icmp/sub/select feeding a scalar
; store split.
; Real lifted IR: pre-zlib-clang-o2.ll.ir-clean.ll#sub_140002AE0 bb_140002c30
; (same shape in sub_140004200 and pre-zlib-gcc-o3.ll#sub_140006EB0).
; RUN: %revenant-opt -passes=revenant-devectorize -S %s | %FileCheck %s

define void @sat_sub(ptr %p, ptr %q) {
entry:
  %v = load <8 x i16>, ptr %p, align 1
  %b = load <8 x i16>, ptr %q, align 1
  %c = icmp ult <8 x i16> %v, %b
  %d = sub <8 x i16> %v, %b
  %s = select <8 x i1> %c, <8 x i16> zeroinitializer, <8 x i16> %d
  store <8 x i16> %s, ptr %p, align 1
  ret void
}
; CHECK-NOT: <8 x i1>
; CHECK-NOT: icmp ult <8 x i16>
; Each lane becomes icmp + sub + select feeding a scalar store.
; CHECK-COUNT-8: icmp ult i16
; CHECK-COUNT-8: select i1
; CHECK-COUNT-8: store i16
; CHECK: ret void
