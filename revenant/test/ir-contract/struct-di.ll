; struct field access; the record shape lives in DI, not IR types
source_filename = "point.exe"
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

define i32 @sub_140001600(ptr %p) #0 !dbg !10 {
bb_140001600:
  %self = alloca ptr, align 8, !revenant.va !14
  #dbg_declare(ptr %self, !20, !DIExpression(), !21)
  store ptr %p, ptr %self, align 8, !revenant.va !15
  %x = load i32, ptr %p, align 4, !revenant.va !16
  %yp = getelementptr inbounds i8, ptr %p, i64 4, !revenant.va !17
  %y = load i32, ptr %yp, align 4, !revenant.va !18
  %s = add i32 %x, %y, !revenant.va !19
  ret i32 %s, !revenant.va !22
}

attributes #0 = { "revenant.va"="0x140001600" }

!llvm.dbg.cu = !{!2}
!llvm.module.flags = !{!0, !1}

!0 = !{i32 4, !"revenant.lang", !"c"}
!1 = !{i32 2, !"Debug Info Version", i32 3}
!2 = distinct !DICompileUnit(language: DW_LANG_C, file: !3, producer: "revenant", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug)
!3 = !DIFile(filename: "point.exe", directory: "revenant")
!10 = distinct !DISubprogram(name: "sub_140001600", linkageName: "sub_140001600", scope: !3, file: !3, line: 1, type: !11, scopeLine: 1, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !2)
!11 = !DISubroutineType(types: !12)
!12 = !{!13}
!13 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!14 = !{i64 u0x140001600}
!15 = !{i64 u0x140001604}
!16 = !{i64 u0x140001608}
!17 = !{i64 u0x14000160c}
!18 = !{i64 u0x140001610}
!19 = !{i64 u0x140001614}
!20 = !DILocalVariable(name: "p", arg: 1, scope: !10, file: !3, line: 1, type: !23)
!21 = !DILocation(line: 1, column: 1, scope: !10)
!22 = !{i64 u0x140001617}
!23 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !24, size: 64)
!24 = distinct !DICompositeType(tag: DW_TAG_structure_type, name: "Point", scope: !3, file: !3, line: 1, size: 64, elements: !25)
!25 = !{!26, !27}
!26 = !DIDerivedType(tag: DW_TAG_member, name: "x", scope: !24, file: !3, line: 2, baseType: !13, size: 32)
!27 = !DIDerivedType(tag: DW_TAG_member, name: "y", scope: !24, file: !3, line: 3, baseType: !13, size: 32, offset: 32)
