; RUN: %revenant-opt -passes=revenant-untailrec -S %s | %FileCheck %s

; Self-loop header (the shape clang -O2 leaves for @tail_gcd): one block,
; phis fed by its own body, backedge from its own branch. The pass splits
; a latch out, then rebuilds the recursive call.
define i32 @selfloop(i32 %a, i32 %b) {
; CHECK-LABEL: define i32 @selfloop(
entry:
  %c = icmp eq i32 %b, 0
  br i1 %c, label %exit, label %loop
loop:
  %pb = phi i32 [%b, %entry], [%nb, %loop]
  %pa = phi i32 [%a, %entry], [%pb, %loop]
  %nb = srem i32 %pa, %pb
  %done = icmp eq i32 %nb, 0
  br i1 %done, label %exit2, label %loop
exit:
  ret i32 %a
exit2:
  ret i32 %pb
; CHECK: call i32 @selfloop(
; CHECK: ret i32
}
