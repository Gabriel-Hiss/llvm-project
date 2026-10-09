; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-locals-to-slots -S %s | %FileCheck %s
;
; A one-shot merge value that is neither debug-backed nor loop-carried stays
; in SSA form instead of becoming a register-named stack slot.

define i32 @temporary(i1 %cond) {
; CHECK-LABEL: define i32 @temporary(
; CHECK-NOT: alloca i32
; CHECK: %value = phi i32 [ 1, %left ], [ 2, %right ]
; CHECK: %sum = add i32 %value, 3
; CHECK: ret i32 %sum
entry:
  br i1 %cond, label %left, label %right

left:
  br label %merge

right:
  br label %merge

merge:
  %value = phi i32 [ 1, %left ], [ 2, %right ]
  %sum = add i32 %value, 3
  ret i32 %sum
}
