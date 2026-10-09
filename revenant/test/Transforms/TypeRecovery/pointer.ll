; RUN: revenant-opt -passes=revenant-types -S %s | FileCheck %s
;
; A single-offset pointer base stays a scalar pointer (no struct); the
; opaque inttoptr call keeps its !revenant.opaque marker.

; CHECK-LABEL: define i32 @sub_140003000(
; CHECK: %v0 = load i32, ptr %p, align 4, !revenant.va !{{[0-9]+}}
; CHECK: %q = call ptr @revenant.asm.lea(ptr %p), !dbg !{{[0-9]+}}, !revenant.va !{{[0-9]+}}, !revenant.opaque !{{[0-9]+}}
; CHECK-DAG: !DILocalVariable(name: "a0", arg: 1, scope: !{{[0-9]+}}, file: !{{[0-9]+}}, line: 1, type: !{{[0-9]+}})
; CHECK-DAG: !DIBasicType(name: "int64_t", size: 64, encoding: DW_ATE_signed)
declare ptr @revenant.asm.lea(ptr)

define i32 @sub_140003000(i64 %a0) !revenant.va !0 {
bb_140003000:
  %p = inttoptr i64 %a0 to ptr, !revenant.va !1
  %v0 = load i32, ptr %p, align 4, !revenant.va !2
  %q = call ptr @revenant.asm.lea(ptr %p), !revenant.va !3, !revenant.opaque !4
  %v1 = load i32, ptr %q, align 4, !revenant.va !3
  ret i32 %v1
}

!0 = !{i64 140003000}
!1 = !{i64 140003003}
!2 = !{i64 140003006}
!3 = !{i64 140003009}
!4 = !{}
