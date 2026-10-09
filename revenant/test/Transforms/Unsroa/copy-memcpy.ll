; RUN: %revenant-opt -passes=revenant-unsroa -S %s | %FileCheck %s
;
; Scalarized struct copy back to memcpy, gated on aggregate evidence:
; the run must be explainable as one source struct copy (whole object or
; a field-aligned window). The opaque-pointer shape that used to fold
; (byte-dispatch code, e.g. the CRT memcpy size-switch cases in c-o2
; sub_14006C7C0) now stays scalar.
;
; Corpus: c-o2 sub_14006C7C0 (CRT memcpy expansion, negative case) and
; the scalarized `*dst = *src` shape SROA leaves for small aggregates
; passed around (positive cases).

target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"

%struct.Pair = type { i32, i32 }

; Two whole-object copies between same-layout aggregate slots fold.
define void @copy_pair(ptr %dst, ptr %src) {
; CHECK-LABEL: define void @copy_pair(
bb:
  %s = alloca %struct.Pair, align 4
  %d = alloca %struct.Pair, align 4
  %v0 = load i32, ptr %s, align 4, !revenant.va !{i64 8193}
  store i32 %v0, ptr %d, align 4, !revenant.va !{i64 8195}
  %s1 = getelementptr inbounds i8, ptr %s, i64 4, !revenant.va !{i64 8196}
  %v1 = load i32, ptr %s1, align 4, !revenant.va !{i64 8197}
  %d1 = getelementptr inbounds i8, ptr %d, i64 4, !revenant.va !{i64 8198}
  store i32 %v1, ptr %d1, align 4, !revenant.va !{i64 8199}
; CHECK: call void @llvm.memcpy
; CHECK: i64 8
  ret void
}

; A field-aligned window of same-offset struct-typed bases also folds.
%struct.Outer = type { i64, %struct.Pair, i64 }
define void @copy_inner() {
; CHECK-LABEL: define void @copy_inner(
bb:
  %o = alloca %struct.Outer, align 8
  %d = alloca %struct.Outer, align 8
  %i0 = getelementptr inbounds i8, ptr %o, i64 8, !revenant.va !{i64 8200}
  %v0 = load i32, ptr %i0, align 4, !revenant.va !{i64 8201}
  %d0 = getelementptr inbounds i8, ptr %d, i64 8, !revenant.va !{i64 8202}
  store i32 %v0, ptr %d0, align 4, !revenant.va !{i64 8203}
  %i1 = getelementptr inbounds i8, ptr %o, i64 12, !revenant.va !{i64 8204}
  %v1 = load i32, ptr %i1, align 4, !revenant.va !{i64 8205}
  %d1 = getelementptr inbounds i8, ptr %d, i64 12, !revenant.va !{i64 8206}
  store i32 %v1, ptr %d1, align 4, !revenant.va !{i64 8207}
; CHECK: call void @llvm.memcpy
; CHECK: i64 8
  ret void
}

; Byte-dispatch shape (CRT memcpy size-switch, c-o2 sub_14006C7C0):
; opaque inttoptr roots, arbitrary widths, no aggregate evidence.
; CHECK-LABEL: define void @copy_dispatch(
; CHECK-NOT: call void @llvm.memcpy
; CHECK: ret void
define void @copy_dispatch(i64 %d0, i64 %s0) {
bb:
  %src = inttoptr i64 %s0 to ptr, !revenant.va !{i64 8300}
  %dst = inttoptr i64 %d0 to ptr, !revenant.va !{i64 8301}
  %v0 = load i64, ptr %src, align 1, !revenant.va !{i64 8302}
  store i64 %v0, ptr %dst, align 1, !revenant.va !{i64 8303}
  %s1 = getelementptr i8, ptr %src, i64 8, !revenant.va !{i64 8304}
  %v1 = load i8, ptr %s1, align 1, !revenant.va !{i64 8305}
  %d1 = getelementptr i8, ptr %dst, i64 8, !revenant.va !{i64 8306}
  store i8 %v1, ptr %d1, align 1, !revenant.va !{i64 8307}
  ret void
}
