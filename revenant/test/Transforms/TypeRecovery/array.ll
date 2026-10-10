; RUN: %revenant-opt -passes=revenant-types -S %s | %FileCheck %s
;
; Fixed-size local arrays retain their extent in debug metadata.

; CHECK-LABEL: define i32 @array_local(
; CHECK-DAG: !DILocalVariable(name: "values", scope: !{{[0-9]+}}, file: !{{[0-9]+}}, line: 1, type: ![[ARRAY:[0-9]+]])
; CHECK-DAG: [[INT:![0-9]+]] = !DIBasicType(name: "int32_t", size: 32, encoding: DW_ATE_signed)
; CHECK-DAG: ![[ARRAY]] = !DICompositeType(tag: DW_TAG_array_type, baseType: [[INT]], size: 128, align: 128, elements: ![[SUBRANGES:[0-9]+]])
; CHECK-DAG: ![[SUBRANGES]] = !{![[RANGE:[0-9]+]]}
; CHECK-DAG: ![[RANGE]] = !DISubrange(count: 4)
define i32 @array_local(i32 %n) !revenant.va !0 {
entry:
  %values = alloca [4 x i32], align 16
  %slot = getelementptr inbounds [4 x i32], ptr %values, i64 0, i64 0
  store i32 %n, ptr %slot, align 4, !revenant.va !1
  %value = load i32, ptr %slot, align 4, !revenant.va !2
  ret i32 %value
}

!0 = !{i64 140005000}
!1 = !{i64 140005003}
!2 = !{i64 140005007}
