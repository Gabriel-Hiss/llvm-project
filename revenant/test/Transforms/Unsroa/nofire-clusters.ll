; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unsroa -S %s | %FileCheck %s
;
; Rebuilder E vetoes, all taken from c-o2 lifted shapes:
;  - a dense uniform run (unrolled array window, not a struct),
;  - a vtable-style single offset (needs >=2 distinct offsets),
;  - copy-routine roots: two inttoptrs of the same argument are per-case
;    windows of a size-switch memcpy, not two objects.
;
; CHECK-NOT: struct.agg

target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"

; Four adjacent i32s at stride 4: reads as an unrolled array (dot4),
; not a 4-field struct.
define i32 @dot4(i64 %0) {
bb:
  %base = inttoptr i64 %0 to ptr
  %e0 = load i32, ptr %base, align 1
  %g1 = getelementptr i32, ptr %base, i64 1
  %e1 = load i32, ptr %g1, align 1
  %g2 = getelementptr i32, ptr %base, i64 2
  %e2 = load i32, ptr %g2, align 1
  %g3 = getelementptr i32, ptr %base, i64 3
  %e3 = load i32, ptr %g3, align 1
  %s0 = add i32 %e0, %e1
  %s1 = add i32 %e2, %e3
  %s = add i32 %s0, %s1
  ret i32 %s
}

; An adjacent same-type pair: two i64s could be a {i64,i64} or two
; array slots; two-element evidence is too weak to claim a field set.
define i64 @pair(i64 %0) {
bb:
  %base = inttoptr i64 %0 to ptr
  %v = load i64, ptr %base, align 1
  %g = getelementptr i64, ptr %base, i64 1
  %w = load i64, ptr %g, align 1
  %r = add i64 %v, %w
  ret i64 %r
}

; Two inttoptr roots of ONE argument, one per case: the size-switch
; memcpy shape in c-o2 sub_14006C7C0. Each root alone would cluster,
; but both scalarize to the same value, so they are per-case windows.
define void @dispatch(i64 %0, i64 %1, i1 %sel) {
entry:
  br i1 %sel, label %case_a, label %case_b
case_a:
  %src.a = inttoptr i64 %0 to ptr
  %v0 = load i64, ptr %src.a, align 1
  %g = getelementptr i16, ptr %src.a, i64 2
  %v1 = load i16, ptr %g, align 1
  %s0 = add i64 %v0, 1
  br label %out
case_b:
  %src.b = inttoptr i64 %0 to ptr
  %w0 = load i32, ptr %src.b, align 1
  %gb = getelementptr i8, ptr %src.b, i64 4
  %w1 = load i16, ptr %gb, align 1
  %s1 = zext i16 %w1 to i64
  br label %out
out:
  ret void
}
