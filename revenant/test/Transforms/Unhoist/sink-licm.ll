; RUN: %revenant-opt -passes=revenant-unhoist -S %s | %FileCheck %s
;
; Corpus: c-clang-o1 main.pre.ll @sum_vec (loop-invariant bound load
; hoisted by LICM into the preheader). After LICM the loop header reads
; the bound from the preheader; ir-pre reloads it in the body each
; iteration. The pass sinks the hoisted load back into the body and
; drops the preheader copy. Seed loads feeding only header phis stay:
; a body clone cannot feed a phi's entry edge.
;
; CHECK-LABEL: define i32 @sum_vec(
define i32 @sum_vec(ptr %a, i32 %n) {
entry:
  %n.addr = alloca i32, align 4
  %s.addr = alloca i32, align 4
  %i.addr = alloca i32, align 4
  store i32 %n, ptr %n.addr, align 4
  store i32 0, ptr %s.addr, align 4
  store i32 0, ptr %i.addr, align 4
; LICM output shape: bound hoisted to the entry block.
; CHECK: entry:
; CHECK-NOT: %bound = load i32, ptr %n.addr
  %bound = load i32, ptr %n.addr, align 4
  br label %loop

loop:                                             ; preds = %latch, %entry
  %s = phi i32 [ %s.next, %latch ], [ 0, %entry ]
  %i = phi i32 [ %i.next, %latch ], [ 0, %entry ]
; The bound reload is sunk back into the loop body.
; CHECK: loop:
; CHECK: [[SUNK:%.*]] = load i32, ptr %n.addr
; CHECK: %c = icmp slt i32 %i, [[SUNK]]
  %c = icmp slt i32 %i, %bound
  br i1 %c, label %body, label %exit

body:                                             ; preds = %loop
  %p = getelementptr i32, ptr %a, i32 %i
  %v = load i32, ptr %p, align 4
  %s.next = add i32 %s, %v
  br label %latch

latch:                                            ; preds = %body
  %i.next = add i32 %i, 1
  br label %loop

exit:                                             ; preds = %loop
  ret i32 %s
}
