; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unsroa -S %s | %FileCheck %s
;
; Scalarized struct copy back to memcpy: two adjacent i32 load/store pairs
; between two bases fold into one llvm.memcpy of 8 bytes. Mirrors the
; scalarized form SROA leaves for small aggregates passed by value.

target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"

define void @copy4(ptr noalias %dst, ptr noalias %src) {
; CHECK-LABEL: define void @copy4(
bb:
  %v0 = load i32, ptr %src, align 4, !revenant.va !{i64 8193}
  store i32 %v0, ptr %dst, align 4, !revenant.va !{i64 8195}
  %s1 = getelementptr inbounds i8, ptr %src, i64 4, !revenant.va !{i64 8196}
  %v1 = load i32, ptr %s1, align 4, !revenant.va !{i64 8197}
  %d1 = getelementptr inbounds i8, ptr %dst, i64 4, !revenant.va !{i64 8198}
  store i32 %v1, ptr %d1, align 4, !revenant.va !{i64 8199}
; CHECK: call void @llvm.memcpy
; CHECK: i64 8
  ret void
}
