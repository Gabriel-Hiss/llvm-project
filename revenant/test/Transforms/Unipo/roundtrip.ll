; Forward round-trip: stock deadargelim turns the pre shape (unused i32
; return, dead at every caller) into the post shape (void), and
; revenant-unipo inverts it back.
; RUN: %opt -passes=deadargelim -S %s -o - | %opt -load-pass-plugin=%plugin -passes=revenant-unipo -S - | %FileCheck %s

; CHECK: define internal i32 @roundtrip(ptr %a, i32 %n)
; CHECK: ret i32 %v
define internal i32 @roundtrip(ptr %a, i32 %n) !dbg !5 {
entry:
  %v = call i32 @produces(ptr %a, i32 %n)
  ret i32 %v
}

define void @caller(ptr %a, i32 %n) {
entry:
  call i32 @roundtrip(ptr %a, i32 %n)
  ret void
}

declare i32 @produces(ptr, i32)

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!6}
!0 = distinct !DICompileUnit(language: DW_LANG_C, file: !1, producer: "Revenant", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug)
!1 = !DIFile(filename: "test.c", directory: "/")
!2 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!3 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: null, size: 64)
!4 = !DISubroutineType(types: !{!2, !3, !2})
!5 = distinct !DISubprogram(name: "roundtrip", linkageName: "roundtrip", scope: !1, file: !1, line: 1, type: !4, scopeLine: 1, spFlags: DISPFlagDefinition, unit: !0)
!6 = !{i32 2, !"Debug Info Version", i32 3}
