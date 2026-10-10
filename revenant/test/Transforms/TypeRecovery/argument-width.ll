; RUN: %revenant-opt -passes=revenant-types -S %s | %FileCheck %s
;
; Minimized from c-clang-o2.exe.decompiled.ir-final.ll, sub_140001000.
; Its first lifted parameter is i64 but only reaches the body through i32.
;
; CHECK-LABEL: define i32 @sub_140001000(
; CHECK-DAG: [[INT:![0-9]+]] = !DIBasicType(name: "int32_t", size: 32, encoding: DW_ATE_signed)
; CHECK-DAG: [[SIGARRAY:![0-9]+]] = !{[[INT]], [[INT]], [[INT]]}
; CHECK-DAG: !DISubroutineType(types: [[SIGARRAY]])
; CHECK-DAG: !DILocalVariable(name: "a0", arg: 1, scope: !{{[0-9]+}}, file: !{{[0-9]+}}, line: 1, type: [[INT]])
; CHECK-DAG: !DILocalVariable(name: "a1", arg: 2, scope: !{{[0-9]+}}, file: !{{[0-9]+}}, line: 1, type: [[INT]])
define i32 @sub_140001000(i64 %a0, i32 %a1) !revenant.va !0 {
entry:
  %narrow = trunc i64 %a0 to i32, !revenant.va !1
  %product = mul i32 %a1, %narrow, !revenant.va !1
  ret i32 %product, !revenant.va !2
}

!0 = !{i64 140001000}
!1 = !{i64 140001012}
!2 = !{i64 140001031}
