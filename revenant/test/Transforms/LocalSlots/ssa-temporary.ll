; RUN: %revenant-opt -passes=revenant-locals-to-slots -S %s | %FileCheck %s
;
; -O0 gives every named local a slot, including a merge value that lives in an
; acyclic code. Any PHI web therefore becomes one memory slot with a store on
; each incoming edge and a load at each use, not a register-named temporary.

define i32 @temporary(i1 %cond) {
; CHECK-LABEL: define i32 @temporary(
; CHECK: %cond.addr = alloca i1
; CHECK: %v_value = alloca i32
; CHECK: store i1 %cond, ptr %cond.addr
; CHECK-NOT: phi i32
entry:
; CHECK: br i1 {{.*}}, label %left, label %right
  br i1 %cond, label %left, label %right

left:
; CHECK: store i32 1, ptr %v_value
  br label %merge

right:
; CHECK: store i32 2, ptr %v_value
  br label %merge

merge:
; CHECK: %[[VALUE:[0-9]+]] = load i32, ptr %v_value
; CHECK: %sum = add i32 %[[VALUE]], 3
; CHECK: ret i32 %sum
  %value = phi i32 [ 1, %left ], [ 2, %right ]
  %sum = add i32 %value, 3
  ret i32 %sum
}
