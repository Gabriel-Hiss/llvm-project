; RUN: %revenant-opt -passes=revenant-reroll -S %s | %FileCheck %s
; Evenly-spaced straight-line windows (offsets 0, 3, 6, 9) synthesize a
; counted loop: every lane sits exactly w strides past window 0, so the
; loop visits precisely the elements the original touched.

define i32 @sum_even(ptr %data, i32 %seed) {
entry:
  %p0 = getelementptr i32, ptr %data, i64 0
  %x0 = load i32, ptr %p0, align 4
  %s0 = add i32 %seed, %x0
  %p1 = getelementptr i32, ptr %data, i64 3
  %x1 = load i32, ptr %p1, align 4
  %s1 = add i32 %s0, %x1
  %p2 = getelementptr i32, ptr %data, i64 6
  %x2 = load i32, ptr %p2, align 4
  %s2 = add i32 %s1, %x2
  %p3 = getelementptr i32, ptr %data, i64 9
  %x3 = load i32, ptr %p3, align 4
  %s3 = add i32 %s2, %x3
  ret i32 %s3
}

; CHECK: define i32 @sum_even
; CHECK: unroll:
; CHECK: phi i64 [ 0, %entry ], [ %u0inext, %unroll ]
; CHECK: icmp ult {{.*}} 4
; CHECK: br i1 {{.*}}, label %unroll, label %unroll.exit
; The three dropped windows are gone; only the first load survives in-loop.
; CHECK: unroll.exit:
; CHECK: ret i32
