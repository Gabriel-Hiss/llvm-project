; RUN: %revenant-opt -passes=revenant-refold -S %s | %FileCheck %s
; Optimized IR carries min/max, abs, saturation and three-way compares as
; intrinsics; cleanup expanded them to compare and select for the source.
; The ir-final storage folds them back, except in unoptimized functions.
; An atomic update whose only use tests its result is the flag-producing
; x86 intrinsic AtomicExpand leaves in the pre-isel IR.

; CHECK-LABEL: define i1 @release(
; CHECK-NEXT: %[[CC:.*]] = call i8 @llvm.x86.atomic.sub.cc.i64(ptr %p, i64 1, i32 4)
; CHECK-NEXT: %last = trunc i8 %[[CC]] to i1
; CHECK-NEXT: ret i1 %last
define i1 @release(ptr %p) {
  %old = atomicrmw sub ptr %p, i64 1 seq_cst, align 8
  %last = icmp eq i64 %old, 1
  ret i1 %last
}

; The old value is read beyond the compare: it stays an atomicrmw.
; CHECK-LABEL: define i64 @kept(
; CHECK: atomicrmw sub
define i64 @kept(ptr %p) {
  %old = atomicrmw sub ptr %p, i64 1 seq_cst, align 8
  %last = icmp eq i64 %old, 1
  %r = select i1 %last, i64 0, i64 %old
  ret i64 %r
}

; CHECK-LABEL: define i32 @umin(
; CHECK: %r = call i32 @llvm.umin.i32(i32 %a, i32 %b)
; CHECK-NOT: select
define i32 @umin(i32 %a, i32 %b) {
  %c = icmp ult i32 %a, %b
  %r = select i1 %c, i32 %a, i32 %b
  ret i32 %r
}

; CHECK-LABEL: define i32 @smax_swapped(
; CHECK: call i32 @llvm.smax.i32(i32 %a, i32 %b)
define i32 @smax_swapped(i32 %a, i32 %b) {
  %c = icmp slt i32 %a, %b
  %r = select i1 %c, i32 %b, i32 %a
  ret i32 %r
}

; CHECK-LABEL: define i32 @abs(
; CHECK: call i32 @llvm.abs.i32(i32 %a, i1 false)
define i32 @abs(i32 %a) {
  %c = icmp slt i32 %a, 0
  %n = sub i32 0, %a
  %r = select i1 %c, i32 %n, i32 %a
  ret i32 %r
}

; CHECK-LABEL: define i32 @usub_sat(
; CHECK: call i32 @llvm.usub.sat.i32(i32 %a, i32 %b)
define i32 @usub_sat(i32 %a, i32 %b) {
  %c = icmp ult i32 %a, %b
  %d = sub i32 %a, %b
  %r = select i1 %c, i32 0, i32 %d
  ret i32 %r
}

; CHECK-LABEL: define i8 @scmp(
; CHECK: call i8 @llvm.scmp.i8.i32(i32 %a, i32 %b)
; CHECK-NOT: icmp
define i8 @scmp(i32 %a, i32 %b) {
  %lt = icmp slt i32 %a, %b
  %gt = icmp sgt i32 %a, %b
  %inner = select i1 %gt, i8 1, i8 0
  %r = select i1 %lt, i8 -1, i8 %inner
  ret i8 %r
}

; CHECK-LABEL: define i32 @unoptimized(
; CHECK-NEXT: %c = icmp ult i32 %a, %b
; CHECK-NEXT: %r = select i1 %c, i32 %a, i32 %b
define i32 @unoptimized(i32 %a, i32 %b) "revenant.o0" {
  %c = icmp ult i32 %a, %b
  %r = select i1 %c, i32 %a, i32 %b
  ret i32 %r
}
