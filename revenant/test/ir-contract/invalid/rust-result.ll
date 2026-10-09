; BROKEN: revenant.lang has illegal value "go"
source_filename = "res.exe"
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

define { i64, i64 } @"_ZN4demo7compute17hdeadbeef0000E"(i64 %x) #0 !dbg !10 {
bb_140001800:
  %ok = icmp sge i64 %x, 0, !revenant.va !14
  br i1 %ok, label %bb_140001810, label %bb_140001820, !revenant.va !15, !revenant.region !16
bb_140001810:
  %v = insertvalue { i64, i64 } { i64 0, i64 undef }, i64 %x, 1, !revenant.va !17
  ret { i64, i64 } %v, !revenant.va !18
bb_140001820:
  %e = insertvalue { i64, i64 } { i64 1, i64 undef }, i64 5, 1, !revenant.va !19
  ret { i64, i64 } %e, !revenant.va !20
}

attributes #0 = { "revenant.va"="0x140001800" }

!llvm.dbg.cu = !{!2}
!llvm.module.flags = !{!0, !1}

!0 = !{i32 4, !"revenant.lang", !"go"}
!1 = !{i32 2, !"Debug Info Version", i32 3}
!2 = distinct !DICompileUnit(language: DW_LANG_Rust, file: !3, producer: "revenant", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug)
!3 = !DIFile(filename: "res.exe", directory: "revenant")
!10 = distinct !DISubprogram(name: "demo::compute", linkageName: "_ZN4demo7compute17hdeadbeef0000E", scope: !3, file: !3, line: 1, type: !11, scopeLine: 1, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !2)
!11 = !DISubroutineType(types: !12)
!12 = !{!23}
!14 = !{i64 u0x140001804}
!15 = !{i64 u0x140001808}
!16 = !{!"if", i32 2}
!17 = !{i64 u0x140001810}
!18 = !{i64 u0x140001818}
!19 = !{i64 u0x140001820}
!20 = !{i64 u0x140001828}
!23 = distinct !DICompositeType(tag: DW_TAG_enumeration_type, name: "Result<i64, i64>", scope: !3, file: !3, line: 1, size: 128, elements: !24)
!24 = !{!25, !26}
!25 = !DIEnumerator(name: "Ok", value: 0)
!26 = !DIEnumerator(name: "Err", value: 1)
