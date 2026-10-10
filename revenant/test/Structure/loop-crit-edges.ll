; RUN: %{rv-structure-tool} %s | %{FileCheck} %s

; Optimized break/continue loops can route both edges through critical-edge
; blocks and keep the induction variable in an alloca.
define i32 @loop_crit_edges(i32 %n) {
entry:
  %i = alloca i32
  %sum = alloca i32
  %empty = icmp slt i32 %n, 1
  br i1 %empty, label %empty.ret, label %init
init:
  store i32 0, ptr %i
  store i32 0, ptr %sum
  br label %test
; continue checks first, then tests whether to break or execute the body.
test:
  %iv = load i32, ptr %i
  %skip = urem i32 %iv, 3
  %continue = icmp eq i32 %skip, 0
  br i1 %continue, label %step.test, label %break.test
step.test:
  %next.test = load i32, ptr %i
  %next = add i32 %next.test, 1
  %done = icmp eq i32 %next, %n
  br i1 %done, label %exit.edge, label %step
step:
  %current = load i32, ptr %i
  %updated = add i32 %current, 1
  store i32 %updated, ptr %i
  br label %test
break.test:
  %break.iv = load i32, ptr %i
  %break = icmp sgt i32 %break.iv, 20
  br i1 %break, label %exit.edge, label %body
body:
  %old.sum = load i32, ptr %sum
  %body.iv = load i32, ptr %i
  %new.sum = add i32 %old.sum, %body.iv
  store i32 %new.sum, ptr %sum
  br label %step.test
empty.ret:
  store i32 0, ptr %sum
  br label %exit
exit.edge:
  br label %exit
exit:
  %result = load i32, ptr %sum
  ret i32 %result
}
; CHECK-LABEL: @loop_crit_edges(
; CHECK: bb_x2:
; CHECK: br i1 %{{.*}}, label %bb_x3, label %bb_x5, !revenant.region ![[COMBO:[0-9]+]]
; CHECK: ![[COMBO]] = !{![[LOOP:[0-9]+]], ![[IF:[0-9]+]]}
; CHECK: ![[LOOP]] = !{!"loop", i32 2, i32 4, i32 8}
