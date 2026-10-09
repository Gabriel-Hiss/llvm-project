; RUN: %revenant-opt -passes=revenant-types -S %s | %FileCheck %s
;
; Signed compare votes signed: the local gets int32_t, and the
; !revenant.region structure annotation on the branch survives.

; CHECK-LABEL: define i32 @sub_140004000(
; CHECK: br i1 %c, label %bb_140004010, label %bb_140004020, !revenant.va !{{[0-9]+}}, !revenant.region !{{[0-9]+}}
; CHECK-DAG: !DIBasicType(name: "int32_t", size: 32, encoding: DW_ATE_signed)
; CHECK: !{{[0-9]+}} = !{!"if"
define i32 @sub_140004000(i32 %a0) !revenant.va !0 {
bb_140004000:
  %v0 = alloca i32, align 4
  store i32 %a0, ptr %v0, align 4, !revenant.va !1
  %v1 = load i32, ptr %v0, align 4, !revenant.va !2
  %c = icmp sgt i32 %v1, 0, !revenant.va !3
  br i1 %c, label %bb_140004010, label %bb_140004020, !revenant.region !5, !revenant.va !3
bb_140004010:
  ret i32 1
bb_140004020:
  ret i32 0
}

!0 = !{i64 140004000}
!1 = !{i64 140004003}
!2 = !{i64 140004006}
!3 = !{i64 140004009}
!5 = !{!"if", i32 2}
