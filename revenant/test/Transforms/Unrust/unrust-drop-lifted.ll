; Corpus: rust-o3 lifted ir-clean. Inlined drop glue at -O3 keeps a
; discriminant guard (`icmp eq len, 0` on a load) around a free arm, and
; the dealloc callee is either the Windows heap import the std allocator
; uses or an unnamed `sub_<VA>` declaration. Two shapes:
;   * @shim_drop_lifted: the free arm only reads the aggregate base, so
;     the guarded region outlines into drop_glue.unrust and the site
;     keeps a call.
;   * @eh_drop_lifted (@sub_1400073C0's shape): the free arm reads a
;     pointer loaded in another block, which the outline cannot reach,
;     so it bails and leaves the CFG alone rather than emitting a
;     dangling reference.
; RUN: %revenant-opt -passes=revenant-unrust -S %s | %FileCheck %s

!llvm.module.flags = !{!0}
!0 = !{i32 4, !"revenant.lang", !"rust"}

declare i64 @GetProcessHeap()
declare i64 @HeapFree(i64, i64, i64)
declare void @sub_140015C2B()

define void @shim_drop_lifted(ptr %agg) {
; CHECK-LABEL: define void @shim_drop_lifted(
entry:
  %len = load i64, ptr %agg, align 8, !revenant.va !{i64 10907}
  %c = icmp eq i64 %len, 0
  br i1 %c, label %live, label %free

free:                                              ; preds = %entry
  %buf = load i64, ptr %agg, align 8
  %heap = call i64 @GetProcessHeap()
  call i64 @HeapFree(i64 %heap, i64 0, i64 %buf)
  br label %live

live:                                              ; preds = %free, %entry
  ret void
; The guarded free moves into a drop_glue.unrust function; the site
; keeps a call with the aggregate base.
; CHECK: call void @drop_glue.unrust(ptr %agg)
}

define void @eh_drop_lifted(ptr %obj) {
; CHECK-LABEL: define void @eh_drop_lifted(
entry:
  %slot = load ptr, ptr %obj, align 8
  %buf = load i64, ptr %slot, align 8
  %c = icmp eq i64 %buf, 0
  br i1 %c, label %live, label %free

free:                                              ; preds = %entry
  %len = load i64, ptr %slot, align 8
  %heap = call i64 @GetProcessHeap()
  call i64 @HeapFree(i64 %heap, i64 0, i64 %len)
  br label %live

live:                                              ; preds = %free, %entry
  call void @sub_140015C2B()
  ret void
; The free arm reads %slot, defined in the entry block: no outline.
; CHECK-NOT: call void @drop_glue.unrust
; The outlined function lands after every original define.
; CHECK: define internal void @drop_glue.unrust(
; CHECK: call i64 @HeapFree(
}
