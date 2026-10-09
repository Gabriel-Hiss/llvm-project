; DI-backed return recovery: errfile's source prototype says i32 even though
; the optimized body retains only the dead value used to return it.
; RUN: %revenant-opt -passes=revenant-unipo -verify-each -S %s | %FileCheck %s

; CHECK-LABEL: define i32 @errfile_like(ptr %a, ptr %b, i32 %n)
; CHECK: ret i32 %v
define void @errfile_like(ptr %a, ptr %b, i32 %n) !dbg !5 {
entry:
  %v = call i32 @produces(ptr %a, ptr %b, i32 %n), !revenant.va !{i64 7000}
  call void @consume(ptr %a, i32 %n), !revenant.va !{i64 7008}
  ret void
}

declare i32 @produces(ptr, ptr, i32)
declare void @consume(ptr, i32)

; No-fire: mixed void/value returns are not an elimination shape.
; CHECK-LABEL: define void @mixed(
; CHECK: ret void
define void @mixed(ptr %a, i1 %c) {
entry:
  br i1 %c, label %t, label %f
t:
  call void @consume(ptr %a, i32 1)
  ret void
f:
  call void @consume(ptr %a, i32 2)
  ret void
}
; Without source return metadata, the dead value alone is not enough evidence.
; CHECK-LABEL: define void @no_return_di()
; CHECK: ret void
define void @no_return_di() {
  %v = add i32 2, 3
  ret void
}

!llvm.module.flags = !{!6}
!0 = distinct !DICompileUnit(language: DW_LANG_C, file: !1, producer: "Revenant", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug)
!1 = !DIFile(filename: "test.c", directory: "/")
!2 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!3 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: null, size: 64)
!4 = !DISubroutineType(types: !{!2, !3, !3, !2})
!5 = distinct !DISubprogram(name: "errfile_like", scope: !1, file: !1, line: 1, type: !4, scopeLine: 1, spFlags: DISPFlagDefinition, unit: !0)
!6 = !{i32 2, !"Debug Info Version", i32 3}
