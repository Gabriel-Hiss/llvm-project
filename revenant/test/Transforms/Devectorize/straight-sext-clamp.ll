; Mask sext + clamp chain (zlib-clang-o2 shape): a vector compare feeds
; sext-from-i1 (-1/0 mask), the mask shuffles wider, a second compare clamps
; against a splat bound with a select, and the result narrows with trunc.
; Real lifted IR: pre-zlib-clang-o2.ll.ir-clean.ll#sub_140002AE0 lines 76-87.
; RUN: %revenant-opt -passes=revenant-devectorize -S %s | %FileCheck %s

define void @mask_clamp(ptr %p, ptr %q) {
entry:
  %a = load <4 x i32>, ptr %p, align 1
  %b = load <4 x i32>, ptr %q, align 1
  %c = icmp slt <4 x i32> %a, %b
  %m = sext <4 x i1> %c to <4 x i32>
  %w = shufflevector <4 x i32> %m, <4 x i32> poison, <8 x i32> <i32 0, i32 1, i32 2, i32 3, i32 0, i32 1, i32 2, i32 3>
  %d = icmp sgt <8 x i32> %w, splat (i32 -32768)
  %s = select <8 x i1> %d, <8 x i32> %w, <8 x i32> splat (i32 -32768)
  %n = trunc <8 x i32> %s to <8 x i16>
  store <8 x i16> %n, ptr %p, align 1
  ret void
}
; Every interior producer scalarizes: sexts, compares, and selects go per-lane.
; CHECK-NOT: sext <4 x i1>
; CHECK: sext i1
; CHECK: icmp s
; CHECK: select i1
; CHECK: ret void
