; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unhoist -S %s | %FileCheck %s
;
; Phase 3: GVN/PRE merge one identical pure computation shared by
; sibling branches into the common predecessor. The pass pushes the
; computation back into each successor that uses it.
;
; CHECK-LABEL: define i32 @cse(
define i32 @cse(i32 %a, i32 %b, i1 %c) {
entry:
; CHECK: entry:
; CHECK-NOT: %s = add i32 %a, %b
; CHECK: br i1 %c, label %left, label %right
  %s = add i32 %a, %b
  br i1 %c, label %left, label %right
left:                                             ; preds = %entry
; CHECK: left:
; CHECK: [[L:%.*]] = add i32 %a, %b
; CHECK: %x = add i32 [[L]], 1
  %x = add i32 %s, 1
  ret i32 %x

right:                                            ; preds = %entry
; CHECK: right:
; CHECK: [[R:%.*]] = add i32 %a, %b
; CHECK: %y = add i32 [[R]], 2
  %y = add i32 %s, 2
  ret i32 %y
}

; CHECK-LABEL: define i32 @cse_shared(
define i32 @cse_shared(i32 %a, i32 %b, i1 %c) {
entry:
; When only one successor uses the hoisted value, the clone still goes
; back to that successor and the predecessor copy drops.
; CHECK: entry:
; CHECK-NOT: %s = add i32 %a, %b
; CHECK: br i1 %c, label %left, label %right
  %s = add i32 %a, %b
  br i1 %c, label %left, label %right

left:                                             ; preds = %entry
; CHECK: left:
; CHECK: [[L:%.*]] = add i32 %a, %b
; CHECK: %x = add i32 [[L]], 1
  %x = add i32 %s, 1
  ret i32 %x

right:                                            ; preds = %entry
; CHECK: right:
; CHECK-NOT: add i32 %a, %b
  ret i32 0
}
