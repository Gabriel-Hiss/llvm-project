; RUN: revenant-opt -passes=revenant-types -S %s | FileCheck %s
;
; Two distinct constant offsets through one base become a DICompositeType
; struct with one member per offset; the GEPs and loads stay as-is.

; CHECK-LABEL: define i32 @sub_140002000(
; CHECK: %v0 = load i32, ptr %f0, align 4, !revenant.va !{{[0-9]+}}
; CHECK: %v1 = load i32, ptr %f8, align 4, !revenant.va !{{[0-9]+}}
; CHECK-DAG: !DICompositeType(tag: DW_TAG_structure_type, name: "rv_struct_sub_140002000_a0"
; CHECK-DAG: !DIDerivedType(tag: DW_TAG_member, name: "field_0"
; CHECK-DAG: !DIDerivedType(tag: DW_TAG_member, name: "field_8"
define i32 @sub_140002000(ptr %a0) !revenant.va !0 {
bb_140002000:
  %f0 = getelementptr i8, ptr %a0, i64 0, !revenant.va !1
  %v0 = load i32, ptr %f0, align 4, !revenant.va !1
  %f8 = getelementptr i8, ptr %a0, i64 8, !revenant.va !2
  %v1 = load i32, ptr %f8, align 4, !revenant.va !2
  %s = add i32 %v0, %v1, !revenant.va !3
  ret i32 %s
}

!0 = !{i64 140002000}
!1 = !{i64 140002003}
!2 = !{i64 140002007}
!3 = !{i64 140002011}
