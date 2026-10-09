; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unsroa -S %s | %FileCheck %s
;
; Scalar alloca group back to an aggregate: two `.sroa.` members fold into
; one aggregate alloca; member accesses become byte GEPs into it.

target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"

define i32 @sroa_group() {
; CHECK-LABEL: define i32 @sroa_group(
bb:
  %p.sroa.0 = alloca i32, align 4, !revenant.va !{i64 16384}
  %p.sroa.1 = alloca i32, align 4, !revenant.va !{i64 16385}
  store i32 3, ptr %p.sroa.0, align 4, !revenant.va !{i64 16386}
  store i32 4, ptr %p.sroa.1, align 4, !revenant.va !{i64 16387}
; CHECK: %p = alloca [8 x i8]
  %a = load i32, ptr %p.sroa.0, align 4, !revenant.va !{i64 16388}
  %b = load i32, ptr %p.sroa.1, align 4, !revenant.va !{i64 16389}
  %s = add i32 %a, %b, !revenant.va !{i64 16390}
  ret i32 %s
}
