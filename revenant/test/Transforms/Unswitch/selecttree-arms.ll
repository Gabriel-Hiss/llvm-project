; RUN: %revenant-opt -passes=revenant-unswitch -S %s | %FileCheck %s
;
; Compare trees spread across both arms of one range check:
; FoldValueComparisonIntoPredecessors pushed the compares out and
; switchToSelect turned each arm into a chain. Both arms default to
; the same constant, so one switch covers the whole value space.
; Corpus: c-clang-o2 / c-clang-o2avx2 @switch_sparse 0x1400012A0
; (cases 1, -5, 100, 10000; default 50).

define i32 @sub_1400012A0(i32 %0) {
bb_1400012a0:
  %c = icmp sgt i32 %0, 99
  br i1 %c, label %bb_1400012b5, label %bb_1400012a5

bb_1400012a5:                                     ; preds = %bb_1400012a0
  %switch.selectcmp = icmp eq i32 %0, 1
  %switch.select = select i1 %switch.selectcmp, i32 10, i32 50
  %switch.selectcmp98 = icmp eq i32 %0, -5
  %switch.select99 = select i1 %switch.selectcmp98, i32 40, i32 %switch.select
  br label %common.ret

common.ret:                                       ; preds = %bb_1400012a5, %bb_1400012b5
  %common.ret.op = phi i32 [ %switch.select103, %bb_1400012b5 ], [ %switch.select99, %bb_1400012a5 ]
  ret i32 %common.ret.op

bb_1400012b5:                                     ; preds = %bb_1400012a0
  %switch.selectcmp100 = icmp eq i32 %0, 100
  %switch.select101 = select i1 %switch.selectcmp100, i32 20, i32 50
  %switch.selectcmp102 = icmp eq i32 %0, 10000
  %switch.select103 = select i1 %switch.selectcmp102, i32 30, i32 %switch.select101
  br label %common.ret
}

; CHECK-LABEL: define i32 @sub_1400012A0(
; CHECK:       switch i32 %0, label %common.ret [
; CHECK-DAG:     i32 1, label
; CHECK-DAG:     i32 -5, label
; CHECK-DAG:     i32 100, label
; CHECK-DAG:     i32 10000, label
; CHECK: %common.ret.op = phi i32
; CHECK-SAME: [ 50, %bb_1400012a0 ]
; The arm blocks are gone: one switch replaced the tree.
; CHECK-NOT:   bb_1400012a5:
; CHECK-NOT:   bb_1400012b5:

; A case constant the arm cannot reach blocks the merge: 7 is never
; > 99, so the tree stays. Each arm still folds its own chain, which
; keeps the two switches apart.
define i32 @badarm(i32 %0) {
bb_0:
  %c = icmp sgt i32 %0, 99
  br i1 %c, label %bb_2, label %bb_1

bb_1:
  %cmp1 = icmp eq i32 %0, 1
  %sel1 = select i1 %cmp1, i32 10, i32 50
  br label %join

join:
  %phi = phi i32 [ %sel2, %bb_2 ], [ %sel1, %bb_1 ]
  ret i32 %phi

bb_2:
  %cmp2 = icmp eq i32 %0, 7
  %sel2 = select i1 %cmp2, i32 70, i32 50
  br label %join
}

; CHECK-LABEL: define i32 @badarm(
; The merge is blocked and a single-case chain stays a select: both
; arms survive untouched.
; CHECK: br i1 %c, label %bb_2, label %bb_1
; CHECK: %sel1 = select i1 %cmp1, i32 10, i32 50
; CHECK: %sel2 = select i1 %cmp2, i32 70, i32 50

; Different arm defaults also block the merge.
define i32 @diffdef(i32 %0) {
bb_0:
  %c = icmp sgt i32 %0, 99
  br i1 %c, label %bb_2, label %bb_1

bb_1:
  %cmp1 = icmp eq i32 %0, 1
  %sel1 = select i1 %cmp1, i32 10, i32 50
  br label %join

join:
  %phi = phi i32 [ %sel2, %bb_2 ], [ %sel1, %bb_1 ]
  ret i32 %phi

bb_2:
  %cmp2 = icmp eq i32 %0, 100
  %sel2 = select i1 %cmp2, i32 70, i32 99
  br label %join
}

; CHECK-LABEL: define i32 @diffdef(
; CHECK: br i1 %c, label %bb_2, label %bb_1
; CHECK: %sel1 = select i1 %cmp1, i32 10, i32 50
; CHECK: %sel2 = select i1 %cmp2, i32 70, i32 99
