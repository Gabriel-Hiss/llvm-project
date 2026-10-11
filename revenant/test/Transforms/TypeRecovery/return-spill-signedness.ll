; RUN: %revenant-opt -passes=revenant-types -S %s | %FileCheck %s
;
; Signedness evidence must flow through an alloca when multiple return arms
; store a comparison result before the return load.

; CHECK-DAG: define i32 @signed_spill(
; CHECK-DAG: define i32 @unsigned_spill(
; CHECK-DAG: [[SIGNED:![0-9]+]] = !DIBasicType(name: "int32_t", size: 32, encoding: DW_ATE_signed)
; CHECK-DAG: [[UNSIGNED:![0-9]+]] = !DIBasicType(name: "uint32_t", size: 32, encoding: DW_ATE_unsigned)
; CHECK-DAG: [[SIGNED_TYPES:![0-9]+]] = !{[[SIGNED]], !{{[0-9]+}}, !{{[0-9]+}}}
; CHECK-DAG: !DISubroutineType(types: [[SIGNED_TYPES]])
; CHECK-DAG: [[UNSIGNED_TYPES:![0-9]+]] = !{[[UNSIGNED]], !{{[0-9]+}}, !{{[0-9]+}}}
; CHECK-DAG: !DISubroutineType(types: [[UNSIGNED_TYPES]])

define i32 @signed_spill(i32 %a, i32 %b) !revenant.va !0 {
entry:
  %slot = alloca i32, align 4
  %cmp = icmp slt i32 %a, %b, !revenant.va !1
  br i1 %cmp, label %negative, label %ordered
negative:
  store i32 -1, ptr %slot, align 4, !revenant.va !2
  br label %exit
ordered:
  %greater = icmp sgt i32 %a, %b, !revenant.va !3
  %result = zext i1 %greater to i32, !revenant.va !3
  store i32 %result, ptr %slot, align 4, !revenant.va !3
  br label %exit
exit:
  %ret = load i32, ptr %slot, align 4, !revenant.va !4
  ret i32 %ret, !revenant.va !4
}

define i32 @unsigned_spill(i32 %a, i32 %b) !revenant.va !5 {
entry:
  %slot = alloca i32, align 4
  %cmp = icmp ult i32 %a, %b, !revenant.va !6
  br i1 %cmp, label %small, label %large
small:
  store i32 0, ptr %slot, align 4, !revenant.va !7
  br label %exit
large:
  %greater = icmp ugt i32 %a, %b, !revenant.va !8
  %result = zext i1 %greater to i32, !revenant.va !8
  store i32 %result, ptr %slot, align 4, !revenant.va !8
  br label %exit
exit:
  %ret = load i32, ptr %slot, align 4, !revenant.va !9
  ret i32 %ret, !revenant.va !9
}

!0 = !{i64 140007100}
!1 = !{i64 140007104}
!2 = !{i64 140007108}
!3 = !{i64 140007112}
!4 = !{i64 140007116}
!5 = !{i64 140007120}
!6 = !{i64 140007124}
!7 = !{i64 140007128}
!8 = !{i64 140007132}
!9 = !{i64 140007136}
