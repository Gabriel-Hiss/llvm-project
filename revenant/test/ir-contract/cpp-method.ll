; C++ method with `this` and a virtual call through the vtable.
; Klass's DICompositeType has a leading _vptr member; the call target comes
; from slot 0 of the vtable pointer at offset 0 of the object.
source_filename = "klass.exe"
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"
@"??_7Klass@@6B@" = global [2 x ptr] [ptr @"?Method@Klass@@UEAAHXZ",
                                      ptr @"?Dtor@Klass@@UEAAHXZ"],
                  !revenant.va !30, !dbg !31

declare i32 @"?Dtor@Klass@@UEAAHXZ"(ptr) "revenant.va"="0x140009020"

define i32 @"?Method@Klass@@UEAAHXZ"(ptr %this, i32 %a) #0 !dbg !10 {
bb_140001700:
  %self = alloca ptr, align 8, !revenant.va !14
  #dbg_declare(ptr %self, !20, !DIExpression(), !21)
  %arg = alloca i32, align 4, !revenant.va !15
  #dbg_declare(ptr %arg, !22, !DIExpression(), !23)
  store ptr %this, ptr %self, align 8, !revenant.va !16
  store i32 %a, ptr %arg, align 4, !revenant.va !17
  %obj = load ptr, ptr %self, align 8, !revenant.va !18
  %vt = load ptr, ptr %obj, align 8, !revenant.va !19
  %fp = load ptr, ptr %vt, align 8, !revenant.va !24
  %r = call i32 %fp(ptr %obj), !revenant.va !25
  %b = load i32, ptr %arg, align 4, !revenant.va !26
  %s = add i32 %r, %b, !revenant.va !27
  ret i32 %s, !revenant.va !28
}

attributes #0 = { "revenant.va"="0x140001700" }

!llvm.dbg.cu = !{!2}
!llvm.module.flags = !{!0, !1}

!0 = !{i32 4, !"revenant.lang", !"cpp"}
!1 = !{i32 2, !"Debug Info Version", i32 3}
!2 = distinct !DICompileUnit(language: DW_LANG_C_plus_plus, file: !3, producer: "revenant", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug)
!3 = !DIFile(filename: "klass.exe", directory: "revenant")
!10 = distinct !DISubprogram(name: "Klass::Method", linkageName: "?Method@Klass@@UEAAHXZ", scope: !3, file: !3, line: 1, type: !11, scopeLine: 1, containingType: !33, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !2)
!11 = !DISubroutineType(types: !12)
!12 = !{!13}
!13 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!14 = !{i64 u0x140001700}
!15 = !{i64 u0x140001704}
!16 = !{i64 u0x140001708}
!17 = !{i64 u0x14000170c}
!18 = !{i64 u0x140001710}
!19 = !{i64 u0x140001714}
!20 = !DILocalVariable(name: "this", arg: 1, scope: !10, file: !3, line: 1, type: !32, flags: DIFlagArtificial | DIFlagObjectPointer)
!21 = !DILocation(line: 1, column: 1, scope: !10)
!22 = !DILocalVariable(name: "a", arg: 2, scope: !10, file: !3, line: 1, type: !13)
!23 = !DILocation(line: 1, column: 2, scope: !10)
!24 = !{i64 u0x140001718}
!25 = !{i64 u0x14000171c}
!26 = !{i64 u0x140001721}
!27 = !{i64 u0x140001724}
!28 = !{i64 u0x140001727}
!30 = !{i64 u0x140005200}
!31 = !DIGlobalVariableExpression(var: !40, expr: !DIExpression())
!32 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !33, size: 64)
!33 = distinct !DICompositeType(tag: DW_TAG_class_type, name: "Klass", scope: !3, file: !3, line: 1, size: 64, elements: !34)
!34 = !{!35}
!35 = !DIDerivedType(tag: DW_TAG_member, name: "_vptr", scope: !33, file: !3, line: 1, baseType: !36, size: 64, flags: DIFlagArtificial)
!36 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !37, size: 64)
!37 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !38, size: 64)
!38 = !DISubroutineType(types: !39)
!39 = !{!13}
!40 = distinct !DIGlobalVariable(name: "??_7Klass@@6B@", linkageName: "??_7Klass@@6B@", scope: !3, file: !3, line: 1, type: !41, isLocal: false, isDefinition: true)
!41 = !DICompositeType(tag: DW_TAG_array_type, baseType: !36, size: 128, elements: !42)
!42 = !{!43}
!43 = !DISubrange(count: 2)
