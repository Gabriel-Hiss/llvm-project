; RUN: %revenant-opt -passes=revenant-unsroa -S %s | %FileCheck %s
;
; Split aggregate store back to one store: two extractvalue+store pairs that
; cover every field of %pair fold into a single aggregate store.

target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"

%pair = type { i32, i32 }

define void @spill_pair(ptr %dst, %pair %v) {
; CHECK-LABEL: define void @spill_pair(
bb:
  %f0 = extractvalue %pair %v, 0, !revenant.va !{i64 12288}
  %g0 = getelementptr inbounds %pair, ptr %dst, i32 0, i32 0, !revenant.va !{i64 12289}
  store i32 %f0, ptr %g0, align 4, !revenant.va !{i64 12290}
  %f1 = extractvalue %pair %v, 1, !revenant.va !{i64 12291}
  %g1 = getelementptr inbounds %pair, ptr %dst, i32 0, i32 1, !revenant.va !{i64 12292}
  store i32 %f1, ptr %g1, align 4, !revenant.va !{i64 12293}
; CHECK: store %pair %v, ptr %dst
; CHECK-NOT: extractvalue
  ret void
}
