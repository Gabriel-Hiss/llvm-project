; RUN: %revenant-opt -passes=revenant-unsroa -S %s | %FileCheck %s
;
; Veto: a volatile member blocks its group; nothing folds.

target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"

define i32 @volatile_group() {
; CHECK-LABEL: define i32 @volatile_group(
bb:
  %p.sroa.0 = alloca i32, align 4, !revenant.va !{i64 20480}
  %p.sroa.1 = alloca i32, align 4, !revenant.va !{i64 20481}
; CHECK: %p.sroa.0 = alloca i32, align 4
; CHECK: %p.sroa.1 = alloca i32, align 4
  %a = load volatile i32, ptr %p.sroa.0, align 4, !revenant.va !{i64 20482}
  %b = load i32, ptr %p.sroa.1, align 4, !revenant.va !{i64 20483}
  %s = add i32 %a, %b, !revenant.va !{i64 20484}
  ret i32 %s
}
