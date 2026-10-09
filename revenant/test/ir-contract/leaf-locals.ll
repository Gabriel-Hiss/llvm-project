; leaf function with recovered locals; DI carries names and types
source_filename = "leaf.exe"
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

define i32 @sub_140001000(i32 %a0) #0 !dbg !10 {
bb_140001000:
  %buf = alloca i64, align 8, !revenant.va !14
  #dbg_declare(ptr %buf, !20, !DIExpression(), !21)
  %sum = add i32 %a0, 7, !revenant.va !15
  store i64 0, ptr %buf, align 8, !revenant.va !16
  ret i32 %sum, !revenant.va !17
}

attributes #0 = { "revenant.va"="0x140001000" }

!llvm.dbg.cu = !{!2}
!llvm.module.flags = !{!0, !1}

!0 = !{i32 4, !"revenant.lang", !"c"}
!1 = !{i32 2, !"Debug Info Version", i32 3}
!2 = distinct !DICompileUnit(language: DW_LANG_C, file: !3, producer: "revenant", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug)
!3 = !DIFile(filename: "leaf.exe", directory: "revenant")
!10 = distinct !DISubprogram(name: "sub_140001000", linkageName: "sub_140001000", scope: !3, file: !3, line: 1, type: !11, scopeLine: 1, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !2)
!11 = !DISubroutineType(types: !12)
!12 = !{!13}
!13 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!14 = !{i64 u0x140001000}
!15 = !{i64 u0x140001007}
!16 = !{i64 u0x14000100b}
!17 = !{i64 u0x140001012}
!20 = !DILocalVariable(name: "buf", scope: !10, file: !3, line: 2, type: !22)
!21 = !DILocation(line: 2, column: 1, scope: !10)
!22 = !DIBasicType(name: "long long", size: 64, encoding: DW_ATE_signed)
