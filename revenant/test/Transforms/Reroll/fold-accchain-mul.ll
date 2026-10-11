; RUN: %revenant-opt -passes=revenant-reroll,verify -S %s | %FileCheck %s

; Fixture from c-o2 dot4 (VA 0x140001420), lifted IR as it reaches the
; reroll stage: a 2x-unrolled 4-element dot product where the two
; accumulator PHIs are already merged (`%rdx.0`, `%r8.0`), so both
; windows' muls feed the accumulator chain directly.
;
; The fold has to do three things a plain window drop does not:
;   - the accumulator phis pair per window (a multi-accumulator unroll),
;   - the reduction merge outside the loop collapses to the one kept
;     accumulator (`%narrow154` was `%7 + %4`),
;   - the coupled countdown's entry is rescaled 2 -> 4, because the rolled
;     loop runs twice as many iterations.
define i32 @sub_140001420(ptr %0, ptr %1) {
bb_140001420:
  br label %bb_140001470

bb_140001470:
  %r11.0.i32 = phi i32 [ 2, %bb_140001420 ], [ %8, %bb_140001470 ]
  %r8.0.i32 = phi i32 [ 0, %bb_140001420 ], [ %7, %bb_140001470 ]
  %rdx.0.i32 = phi i32 [ 0, %bb_140001420 ], [ %4, %bb_140001470 ]
  %rax.0 = phi i64 [ 0, %bb_140001420 ], [ %d, %bb_140001470 ]
  %element = getelementptr i32, ptr %0, i64 %rax.0
  %2 = load i32, ptr %element, align 1
  %element155 = getelementptr i32, ptr %1, i64 %rax.0
  %3 = load i32, ptr %element155, align 1
  %narrow = mul i32 %2, %3
  %d = add i64 %rax.0, 2
  %4 = add i32 %rdx.0.i32, %narrow
  %sc92 = shl i64 %d, 2
  %address148 = getelementptr i8, ptr %1, i64 %sc92
  %element2 = getelementptr i32, ptr %address148, i64 -1
  %5 = load i32, ptr %element2, align 1
  %address151 = getelementptr i8, ptr %0, i64 %sc92
  %element3 = getelementptr i32, ptr %address151, i64 -1
  %6 = load i32, ptr %element3, align 1
  %narrow153 = mul i32 %5, %6
  %7 = add i32 %r8.0.i32, %narrow153
  %8 = add i32 %r11.0.i32, -1
  %c137.not = icmp eq i32 %8, 0
  br i1 %c137.not, label %bb_140001493, label %bb_140001470

bb_140001493:
  %narrow154 = add i32 %7, %4
  ret i32 %narrow154
}

; CHECK-LABEL: define i32 @sub_140001420(
; One product window survives; the coupled countdown entry is rescaled by
; the window count (2 * 2) so the rolled loop runs the same number of
; element steps. CHECKs are in block order because FileCheck scans forward;
; the countdown step value is renumbered as the dropped windows are erased.
; CHECK: %r11.0.i32 = phi i32 [ 4, %bb_140001420 ], [ %{{[0-9]+}}, %bb_140001470 ]
; CHECK: %narrow = mul i32 %2, %3
; The element step folds 2 -> 1.
; CHECK: %d = add i64 %rax.0, 1
; The second accumulator merge collapses to the single rolled accumulator.
; CHECK: %4 = add i32 %rdx.0.i32, %narrow
; CHECK: %{{[0-9]+}} = add i32 %r11.0.i32, -1
; CHECK: %c137.not = icmp eq i32 %{{[0-9]+}}, 0
; CHECK: ret i32 %4
; The dropped window's address chain is erased, not left reading the
; divided step.
; CHECK-NOT: %sc92 = shl i64
; CHECK-NOT: %address148 = getelementptr
; CHECK-NOT: %address151 = getelementptr
; CHECK-NOT: %element2 = getelementptr
; CHECK-NOT: %element3 = getelementptr
; The second window's work is gone.
; CHECK-NOT: %narrow153 = mul i32
; The second accumulator PHI and its update are gone.
; CHECK-NOT: %r8.0.i32
