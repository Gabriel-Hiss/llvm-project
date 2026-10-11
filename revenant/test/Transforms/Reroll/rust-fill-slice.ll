; RUN: %revenant-opt -passes=revenant-reroll -S %s | %FileCheck %s
; Real rust-o2 lifted IR: corpus Rust `fill_slice`, VA 0x140001050.
; rustc specializes the source 16-element fill loop into four identical i128
; stores at 0/16/32/48-byte offsets. Rebuild the equivalent contiguous loop.
;
; CHECK-LABEL: define void @fill_slice(ptr %dst)
; CHECK: unroll:
; CHECK: %u0i = phi i64 [ 0, %entry ], [ %u0inext, %unroll ]
; CHECK: %u0a = mul i64 %u0i, 16
; CHECK: %u0addr = getelementptr i8, ptr %dst, i64 %u0a
; CHECK: store i128 237684487598133245014645407747, ptr %u0addr, align 1
; CHECK: %u0inext = add i64 %u0i, 1
; CHECK: %u0c = icmp ult i64 %u0inext, 4
; CHECK: br i1 %u0c, label %unroll, label %unroll.exit
; CHECK: unroll.exit:
; CHECK: ret void

define void @fill_slice(ptr %dst) {
entry:
  store i128 237684487598133245014645407747, ptr %dst, align 1, !revenant.va !0
  %element1 = getelementptr i128, ptr %dst, i64 1
  store i128 237684487598133245014645407747, ptr %element1, align 1, !revenant.va !1
  %element2 = getelementptr i128, ptr %dst, i64 2
  store i128 237684487598133245014645407747, ptr %element2, align 1, !revenant.va !2
  %element3 = getelementptr i128, ptr %dst, i64 3
  store i128 237684487598133245014645407747, ptr %element3, align 1, !revenant.va !3
  ret void
}

@fillbuf = global [64 x i8] zeroinitializer, align 16

define i32 @main() {
entry:
  call void @fill_slice(ptr @fillbuf)
  %v0 = load i128, ptr @fillbuf, align 1
  %p1 = getelementptr i128, ptr @fillbuf, i64 1
  %v1 = load i128, ptr %p1, align 1
  %p2 = getelementptr i128, ptr @fillbuf, i64 2
  %v2 = load i128, ptr %p2, align 1
  %p3 = getelementptr i128, ptr @fillbuf, i64 3
  %v3 = load i128, ptr %p3, align 1
  %s0 = add i128 %v0, %v1
  %s1 = add i128 %s0, %v2
  %s2 = add i128 %s1, %v3
  %result = trunc i128 %s2 to i32
  ret i32 %result
}

!0 = !{i64 5368713303}
!1 = !{i64 5368713306}
!2 = !{i64 5368713310}
!3 = !{i64 5368713314}
