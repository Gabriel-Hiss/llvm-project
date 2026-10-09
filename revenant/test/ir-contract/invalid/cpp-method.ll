; BROKEN: dbg.declare locals but the function has no DISubprogram
source_filename = "klass.exe"
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

define i32 @"?Method@Klass@@UEAAHXZ"(ptr %this, i32 %a) #0 {
bb_140001700:
  %self = alloca ptr, align 8, !revenant.va !14
  #dbg_declare(ptr %self, !20, !DIExpression(), !21)
  %obj = load ptr, ptr %self, align 8, !revenant.va !18
  %vt = load ptr, ptr %obj, align 8, !revenant.va !19
  %fp = load ptr, ptr %vt, align 8, !revenant.va !24
  %r = call i32 %fp(ptr %obj), !revenant.va !25
  ret i32 %r, !revenant.va !28
}

attributes #0 = { "revenant.va"="0x140001700" }

!llvm.dbg.cu = !{!2}
!llvm.module.flags = !{!0, !1}

!0 = !{i32 4, !"revenant.lang", !"cpp"}
!1 = !{i32 2, !"Debug Info Version", i32 3}
!2 = distinct !DICompileUnit(language: DW_LANG_C_plus_plus, file: !3, producer: "revenant", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug)
!3 = !DIFile(filename: "klass.exe", directory: "revenant")
!10 = distinct !DISubprogram(name: "Klass::Method", linkageName: "?Method@Klass@@UEAAHXZ", scope: !3, file: !3, line: 1, type: !11, scopeLine: 1, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !2)
!11 = !DISubroutineType(types: !12)
!12 = !{!13}
!13 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!14 = !{i64 u0x140001700}
!18 = !{i64 u0x140001710}
!19 = !{i64 u0x140001714}
!20 = !DILocalVariable(name: "this", arg: 1, scope: !10, file: !3, line: 1, type: !32, flags: DIFlagArtificial | DIFlagObjectPointer)
!21 = !DILocation(line: 1, column: 1, scope: !10)
!24 = !{i64 u0x140001718}
!25 = !{i64 u0x14000171c}
!28 = !{i64 u0x140001727}
!32 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !13, size: 64)
