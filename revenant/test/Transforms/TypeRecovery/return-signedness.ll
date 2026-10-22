; RUN: %revenant-opt -passes=revenant-types -S %s | %FileCheck %s
;
; A zext of an ordered comparison is 0/1, but return signedness follows the
; predicate; unsigned division also carries unsigned evidence to its result.

; CHECK-DAG: [[SIGNED:![0-9]+]] = !DIBasicType(name: "int32_t", size: 32, encoding: DW_ATE_signed)
; CHECK-DAG: [[UNSIGNED:![0-9]+]] = !DIBasicType(name: "uint32_t", size: 32, encoding: DW_ATE_unsigned)
; CHECK-DAG: [[STYPES:![0-9]+]] = !{[[SIGNED]], !{{[0-9]+}}, !{{[0-9]+}}}
; CHECK-DAG: !DISubroutineType(types: [[STYPES]])
; CHECK-DAG: [[UTYPES:![0-9]+]] = !{[[UNSIGNED]], !{{[0-9]+}}, !{{[0-9]+}}}
; CHECK-DAG: !DISubroutineType(types: [[UTYPES]])
; CHECK-DAG: !DISubroutineType(types: !{{[0-9]+}})

define i32 @signed_compare(i32 %a, i32 %b) !revenant.va !0 {
entry:
  %cmp = icmp slt i32 %a, %b, !revenant.va !1
  %result = zext i1 %cmp to i32, !revenant.va !1
  ret i32 %result, !revenant.va !2
}

define i32 @unsigned_compare(i32 %a, i32 %b) !revenant.va !3 {
entry:
  %cmp = icmp ult i32 %a, %b, !revenant.va !4
  %result = zext i1 %cmp to i32, !revenant.va !4
  ret i32 %result, !revenant.va !5
}

define i32 @unsigned_quotient(i32 %a, i32 %b) !revenant.va !6 {
entry:
  %quot = udiv i32 %a, %b, !revenant.va !7
  ret i32 %quot, !revenant.va !8
}

!0 = !{i64 140007000}
!1 = !{i64 140007004}
!2 = !{i64 140007008}
!3 = !{i64 140007010}
!4 = !{i64 140007014}
!5 = !{i64 140007018}
!6 = !{i64 140007020}
!7 = !{i64 140007024}
!8 = !{i64 140007028}
