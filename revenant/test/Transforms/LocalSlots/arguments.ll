; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-locals-to-slots -S %s | %FileCheck %s
;
; A recovered -O0-style function spills every incoming parameter and uses a
; shared retval slot when the function has more than one return path.

define i32 @choose(i32 %left, i32 %right, i1 %cond) {
; CHECK-LABEL: define i32 @choose(
; CHECK: %left.addr = alloca i32
; CHECK: %right.addr = alloca i32
; CHECK: %cond.addr = alloca i1
; CHECK: %retval = alloca i32
; CHECK: store i32 %left, ptr %left.addr
; CHECK: store i32 %right, ptr %right.addr
; CHECK: store i1 %cond, ptr %cond.addr
entry:
  br i1 %cond, label %yes, label %no

yes:
; CHECK: %[[COND:[0-9]+]] = load i1, ptr %cond.addr
; CHECK: br i1 %[[COND]], label %yes, label %no
; CHECK: %[[LEFT:[0-9]+]] = load i32, ptr %left.addr
; CHECK: store i32 %[[LEFT]], ptr %retval
; CHECK: %[[RESULT:[0-9]+]] = load i32, ptr %retval
; CHECK: ret i32 %[[RESULT]]
  ret i32 %left

no:
  ret i32 %right
}
