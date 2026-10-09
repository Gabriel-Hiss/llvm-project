; RUN: %revenant-opt -passes=revenant-unhoist -S %s | %FileCheck %s
;
; Phase 2: GVN deletes a reload after a store and rewrites its uses to
; the stored value, leaving the load dead. The pass re-arms the dead
; load and moves the post-load uses back onto it.
;
; CHECK-LABEL: define i32 @fwd(
define i32 @fwd(ptr %p) {
entry:
  %slot = alloca i32, align 4
  store i32 41, ptr %slot, align 4
; The dead reload sits after the store to the same slot.
; CHECK: entry:
; CHECK: [[R:%.*]] = load i32, ptr %slot
  %dead = load i32, ptr %slot, align 4
; CHECK: %a = add i32 [[R]], 1
; CHECK: %b = add i32 [[R]], 2
; CHECK: %s = add i32 %a, %b
; CHECK: ret i32 %s
  %a = add i32 41, 1
  %b = add i32 41, 2
  %s = add i32 %a, %b
  ret i32 %s
}

; CHECK-LABEL: define i32 @fwd_live(
define i32 @fwd_live(ptr %p) {
entry:
  %slot = alloca i32, align 4
  store i32 41, ptr %slot, align 4
; A live load is not a GVN-forwarded reload: it stays, and the stored
; value keeps its uses.
; CHECK: entry:
; CHECK: %live = load i32, ptr %slot
; CHECK: %s = add i32 %live, 41
; CHECK: ret i32 %s
  %live = load i32, ptr %slot, align 4
  %s = add i32 %live, 41
  ret i32 %s
}
