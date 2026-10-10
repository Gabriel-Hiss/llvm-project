; RUN: %revenant-opt -passes=revenant-reroll,verify -S %s

; Fixture from c-o2 dot4 (VA 0x140001420), lifted IR as it reaches the
; reroll stage: a 2x-unrolled 4-element dot product where the two
; accumulator PHIs are already merged (`%rdx.0`, `%r8.0`), so both
; windows' muls feed the accumulator chain directly.
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
