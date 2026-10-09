; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-locals-to-slots -S %s | %FileCheck %s
;
; A constant arriving over a critical edge needs a store on that edge only.
; The split block keeps the alternate branch's slot contents unchanged.

define i32 @choose(i1 %cond) {
; CHECK-LABEL: define i32 @choose(
; CHECK: entry.join_crit_edge:
; CHECK: store i32 1, ptr %v_value
; CHECK: br label %join
; CHECK: join:
; CHECK-NOT: phi i32
entry:
  br i1 %cond, label %left, label %join

left:
  br label %join

join:
  %value = phi i32 [ 1, %entry ], [ 2, %left ]
  ret i32 %value
}
