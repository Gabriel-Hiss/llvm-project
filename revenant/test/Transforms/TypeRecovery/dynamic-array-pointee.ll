; RUN: %revenant-opt -passes=revenant-types -S %s | %FileCheck %s
;
; A scalar load through a variable-index GEP provides element-pointee evidence
; even though there is no constant struct-field offset.

; CHECK-LABEL: define i32 @indexed_load(
; CHECK-DAG: [[I32:![0-9]+]] = !DIBasicType(name: "int32_t", size: 32, encoding: DW_ATE_signed)
; CHECK-DAG: !DIDerivedType(tag: DW_TAG_pointer_type, baseType: [[I32]], size: 64)
define i32 @indexed_load(ptr %base, i64 %index) !revenant.va !0 {
entry:
  %element = getelementptr i32, ptr %base, i64 %index, !revenant.va !1
  %value = load i32, ptr %element, align 4, !revenant.va !2
  ret i32 %value, !revenant.va !3
}

!0 = !{i64 140006000}
!1 = !{i64 140006004}
!2 = !{i64 140006008}
!3 = !{i64 140006012}
