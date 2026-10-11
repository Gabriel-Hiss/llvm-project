; Drain-tail fusion stray-pred counterexample.
; S dispatches to the countdown drain or straight into the tail through
; a drain-exit block bb_exit. bb_exit has a SECOND predecessor alt: when
; %alt reaches it the tail must still start at t.start.alt.
; The drain-tail fusion that used to live here was removed outright:
; erasing the drain while starting the tail at the drain end lost the
; drain's [indexStart, indexStart+rem) range for every N not a multiple
; of the tail stride (lli differential diverged), and no tail-start
; choice covers both the drain range and N on the tail's lattice. This
; file stays as the no-mutation witness for the shape: the drain, the
; guard phi with its stray incoming, and the dispatch all survive the
; pass untouched, verify-clean.
; RUN: %revenant-opt -passes=revenant-unloopvec,verify -S %s | %FileCheck %s
; CHECK-LABEL: define{{.*}}i32 @fuse_exitpred
; CHECK: br i1 %hazrem, label %bb_exit, label %bb_drain
; CHECK: %t.start = phi i64 [ %d.idx.next, %bb_drain ], [ 0, %bb_s ], [ 4, %bb_alt ]
define i32 @fuse_exitpred(ptr %dst, ptr %src, i32 %addend, i64 %n, i1 %go.alt) {
entry:
  %rem = and i64 %n, 3
  %hazrem = icmp eq i64 %rem, 0
  br i1 %go.alt, label %bb_alt, label %bb_s

bb_s:                                             ; S dispatch
  br i1 %hazrem, label %bb_exit, label %bb_drain

bb_alt:
  br label %bb_exit

bb_drain:                                         ; countdown drain, step-1 index
  %d.cnt = phi i64 [ %d.cnt.next, %bb_drain ], [ %rem, %bb_s ]
  %d.idx = phi i64 [ %d.idx.next, %bb_drain ], [ 0, %bb_s ]
  %d.p = getelementptr i32, ptr %src, i64 %d.idx
  %d.v = load i32, ptr %d.p, align 4
  %d.a = add i32 %d.v, %addend
  %d.q = getelementptr i32, ptr %dst, i64 %d.idx
  store i32 %d.a, ptr %d.q, align 4
  %d.idx.next = add i64 %d.idx, 1
  %d.cnt.next = add i64 %d.cnt, -1
  %d.done = icmp eq i64 %d.cnt.next, 0
  br i1 %d.done, label %bb_exit, label %bb_drain

bb_exit:                                          ; drain-exit with stray pred
  %t.start = phi i64 [ %d.idx.next, %bb_drain ], [ 0, %bb_s ], [ 4, %bb_alt ]
  br label %bb_tail

bb_tail:                                          ; counted tail, 4-lane unrolled body
  %t.idx = phi i64 [ %t.idx.next, %bb_tail ], [ %t.start, %bb_exit ]
  %t.p0 = getelementptr i32, ptr %src, i64 %t.idx
  %t.v0 = load i32, ptr %t.p0, align 4
  %t.a0 = add i32 %t.v0, %addend
  %t.q0 = getelementptr i32, ptr %dst, i64 %t.idx
  store i32 %t.a0, ptr %t.q0, align 4
  %t.i1 = add i64 %t.idx, 1
  %t.p1 = getelementptr i32, ptr %src, i64 %t.i1
  %t.v1 = load i32, ptr %t.p1, align 4
  %t.a1 = add i32 %t.v1, %addend
  %t.q1 = getelementptr i32, ptr %dst, i64 %t.i1
  store i32 %t.a1, ptr %t.q1, align 4
  %t.i2 = add i64 %t.idx, 2
  %t.p2 = getelementptr i32, ptr %src, i64 %t.i2
  %t.v2 = load i32, ptr %t.p2, align 4
  %t.a2 = add i32 %t.v2, %addend
  %t.q2 = getelementptr i32, ptr %dst, i64 %t.i2
  store i32 %t.a2, ptr %t.q2, align 4
  %t.i3 = add i64 %t.idx, 3
  %t.p3 = getelementptr i32, ptr %src, i64 %t.i3
  %t.v3 = load i32, ptr %t.p3, align 4
  %t.a3 = add i32 %t.v3, %addend
  %t.q3 = getelementptr i32, ptr %dst, i64 %t.i3
  store i32 %t.a3, ptr %t.q3, align 4
  %t.idx.next = add i64 %t.idx, 4
  %t.done = icmp uge i64 %t.idx.next, %n
  br i1 %t.done, label %bb_end, label %bb_tail

bb_end:
  %end.v = load i32, ptr %dst, align 4
  ret i32 %end.v
}
