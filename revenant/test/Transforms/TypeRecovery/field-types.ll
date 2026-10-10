; RUN: %revenant-opt -passes=revenant-types -S %s | %FileCheck %s
;
; Field metadata must use the type evidence from its own memory access. The
; unsigned comparison applies to field_8, not to the earlier field_0 access.

; CHECK-LABEL: define i32 @sub_140004000(
; CHECK-DAG: [[RET:![0-9]+]] = !DIBasicType(name: "int32_t", size: 32, encoding: DW_ATE_signed)
; CHECK-DAG: [[SIGARRAY:![0-9]+]] = !{[[RET]], !{{[0-9]+}}}
; CHECK-DAG: !DISubroutineType(types: [[SIGARRAY]])
; CHECK-DAG: [[UNSIGNED:![0-9]+]] = !DIBasicType(name: "uint32_t", size: 32, encoding: DW_ATE_unsigned)
; CHECK-DAG: !DIDerivedType(tag: DW_TAG_member, name: "field_0", scope: !{{[0-9]+}}, file: !{{[0-9]+}}, line: 1, baseType: [[RET]], size: 32, align: 32)
; CHECK-DAG: !DIDerivedType(tag: DW_TAG_member, name: "field_8", scope: !{{[0-9]+}}, file: !{{[0-9]+}}, line: 1, baseType: [[UNSIGNED]], size: 32, align: 32, offset: 64)
define i32 @sub_140004000(ptr %a0) !revenant.va !0 {
bb_140004000:
  %f0 = getelementptr i8, ptr %a0, i64 0, !revenant.va !1
  %v0 = load i32, ptr %f0, align 4, !revenant.va !1
  %f8 = getelementptr i8, ptr %a0, i64 8, !revenant.va !2
  %v1 = load i32, ptr %f8, align 4, !revenant.va !2
  %signed = icmp slt i32 %v0, 0, !revenant.va !3
  %unsigned = icmp ult i32 %v1, 100, !revenant.va !4
  %s = zext i1 %signed to i32, !revenant.va !3
  %u = zext i1 %unsigned to i32, !revenant.va !4
  %r = add i32 %s, %u, !revenant.va !5
  ret i32 %r
}

!0 = !{i64 140004000}
!1 = !{i64 140004003}
!2 = !{i64 140004007}
!3 = !{i64 140004011}
!4 = !{i64 140004015}
!5 = !{i64 140004019}
