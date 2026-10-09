; Dead-arg restore via debug records: rust-o3 `dot4` post shape keeps
; `!{!3, !4, !4}` (ret + 2 ptr formals) on the DISubprogram while
; the IR is `()`, with `llvm.dbg.value(ptr poison)` markers for the lost
; formals. The pass re-adds the trailing formals; call sites pass poison.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unipo -verify-each -S %s | %FileCheck %s

; CHECK-LABEL: define internal i32 @dot4_like(ptr %ipa0, ptr %ipa1)
; CHECK: ret i32 70
define internal i32 @dot4_like() !dbg !10 {
start:
  call void @llvm.dbg.value(metadata ptr poison, metadata !12, metadata !DIExpression()), !dbg !15
  call void @llvm.dbg.value(metadata ptr poison, metadata !13, metadata !DIExpression()), !dbg !15
  ret i32 70, !dbg !16
}

; CHECK-LABEL: define i32 @user()
; CHECK: call i32 @dot4_like(ptr poison, ptr poison)
; CHECK: ret i32 %r
define i32 @user() {
entry:
  %r = call i32 @dot4_like()
  ret i32 %r
}

declare void @llvm.dbg.value(metadata, metadata, metadata) #0

!llvm.module.flags = !{!0}
!llvm.dbg.cu = !{!1}
!0 = !{i32 2, !"Debug Info Version", i32 3}
!1 = distinct !DICompileUnit(language: DW_LANG_C, file: !2, producer: "test", isOptimized: true, emissionKind: FullDebug)
!2 = !DIFile(filename: "lib.c", directory: "/src")
!3 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!4 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !3, size: 64)
!6 = !DISubroutineType(types: !7)
!7 = !{!3, !4, !4}
!10 = distinct !DISubprogram(name: "dot4_like", linkageName: "dot4_like", scope: !1, file: !2, line: 233, type: !6, scopeLine: 233, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !1)
!12 = !DILocalVariable(name: "a", arg: 1, scope: !10, file: !2, line: 233, type: !4)
!13 = !DILocalVariable(name: "b", arg: 2, scope: !10, file: !2, line: 233, type: !4)
!15 = !DILocation(line: 0, scope: !10)
!16 = !DILocation(line: 239, scope: !10)

attributes #0 = { nounwind readnone speculatable willreturn }
