; RUN: %revenant-opt -passes=revenant-reroll -S %s | %FileCheck %s
; Offsets 0, 3, 7, 20 are not evenly spaced. The prefix 0, 3 is an exact
; 2-window run and would synthesize at K=2, but the scan must never
; truncate 7/3 down to a stride of 3 to reach K=3: that would drop the load
; at offset 7 and visit elements the original never touched. The loop stays
; straight-line with all four loads intact.

define i32 @sum_uneven(ptr %data, i32 %seed) {
entry:
  %p0 = getelementptr i32, ptr %data, i64 0
  %x0 = load i32, ptr %p0, align 4
  %s0 = add i32 %seed, %x0
  %p1 = getelementptr i32, ptr %data, i64 3
  %x1 = load i32, ptr %p1, align 4
  %s1 = add i32 %s0, %x1
  %p2 = getelementptr i32, ptr %data, i64 7
  %x2 = load i32, ptr %p2, align 4
  %s2 = add i32 %s1, %x2
  %p3 = getelementptr i32, ptr %data, i64 20
  %x3 = load i32, ptr %p3, align 4
  %s3 = add i32 %s2, %x3
  ret i32 %s3
}

; No loop is synthesized: every offset survives.
; CHECK: define i32 @sum_uneven
; CHECK: getelementptr i32, ptr %data, i64 0
; CHECK: getelementptr i32, ptr %data, i64 3
; CHECK: getelementptr i32, ptr %data, i64 7
; CHECK: getelementptr i32, ptr %data, i64 20
; CHECK-NOT: unroll:
; CHECK-NOT: phi i64
; CHECK: ret i32

; Same shape behind a forged unroll factor: evidence may only lower the
; minimum window count, never manufacture evenly-spaced lanes that the IR
; does not have. The truncated 7/3 stride must not reach K=3.
define i32 @sum_uneven_forged(ptr %data, i32 %seed) !rev.unroll !{i32 4} {
entry:
  %p0 = getelementptr i32, ptr %data, i64 0
  %x0 = load i32, ptr %p0, align 4
  %s0 = add i32 %seed, %x0
  %p1 = getelementptr i32, ptr %data, i64 3
  %x1 = load i32, ptr %p1, align 4
  %s1 = add i32 %s0, %x1
  %p2 = getelementptr i32, ptr %data, i64 7
  %x2 = load i32, ptr %p2, align 4
  %s2 = add i32 %s1, %x2
  %p3 = getelementptr i32, ptr %data, i64 20
  %x3 = load i32, ptr %p3, align 4
  %s3 = add i32 %s2, %x3
  ret i32 %s3
}

; CHECK: define i32 @sum_uneven_forged
; CHECK: getelementptr i32, ptr %data, i64 0
; CHECK: getelementptr i32, ptr %data, i64 3
; CHECK: getelementptr i32, ptr %data, i64 7
; CHECK: getelementptr i32, ptr %data, i64 20
; CHECK-NOT: unroll:
; CHECK-NOT: phi i64
; CHECK: ret i32
