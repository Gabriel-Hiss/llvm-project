; RUN: revenant-opt -passes=revenant-types -S %s | FileCheck %s
;
; Unsigned compare votes unsigned: the local gets a uint32_t DIBasicType
; and the IR itself is untouched (same add/ret, provenance kept).

define i32 @sub_140001000(i32 %a0) !revenant.va !0 {
; CHECK-LABEL: define i32 @sub_140001000(
bb_140001000:
  %v0 = alloca i32, align 4
  store i32 %a0, ptr %v0, align 4, !revenant.va !1
  %v1 = load i32, ptr %v0, align 4, !revenant.va !2
; CHECK: %v1 = load i32, ptr %v0, align 4, !revenant.va !2
  %c = icmp ult i32 %v1, 100, !revenant.va !3
; CHECK: %c = icmp ult i32 %v1, 100, !revenant.va !3
  %e = zext i1 %c to i32, !revenant.va !3
  ret i32 %e
}

; CHECK-DAG: !DIBasicType(name: "uint32_t", size: 32, encoding: DW_ATE_unsigned)
; CHECK-DAG: !DILocalVariable(name: "v0"
; CHECK-DAG: !DILocalVariable(name: "a0"
; CHECK: !revenant.va = !{!2}

!0 = !{i64 140001000}
!1 = !{i64 140001003}
!2 = !{i64 140001006}
!3 = !{i64 140001009}
