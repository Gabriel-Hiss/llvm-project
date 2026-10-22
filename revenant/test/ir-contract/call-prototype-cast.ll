; Call-site casts: the callee's prototype comes from DI (a struct pointer
; parameter) while the IR call site passes an opaque pointer, the shape the
; lifter leaves on MSVC targets (board line 337 family: C2664/C2665 in C++,
; incompatible pointer types in C). The emitted call must cast the argument
; to the declared parameter type.
source_filename = "call-cast.exe"
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

; Callee: a1 is struct rv_struct_sub_1400077F0_arg0 *, a2 is unsigned char *,
; a3 int. All three are declared through #dbg_declare on allocas, the way the
; lifter carries DISubprogram parameters.
define i32 @sub_1400077F0(ptr %a1, ptr %a2, i32 %a3) #0 !dbg !20 {
bb_1400077F0:
  %a1x = alloca ptr, align 8, !revenant.va !14
  %a2x = alloca ptr, align 8, !revenant.va !15
  %a3x = alloca i32, align 4, !revenant.va !16
  #dbg_declare(ptr %a1x, !26, !DIExpression(), !21)
  #dbg_declare(ptr %a2x, !27, !DIExpression(), !21)
  #dbg_declare(ptr %a3x, !28, !DIExpression(), !21)
  store ptr %a1, ptr %a1x, align 8, !revenant.va !17
  store ptr %a2, ptr %a2x, align 8, !revenant.va !18
  store i32 %a3, ptr %a3x, align 4, !revenant.va !19
  ret i32 %a3, !revenant.va !22
}

; Helper returning an opaque byte pointer.
define ptr @sub_1400075A0(ptr %a1, ptr %a2) #0 !dbg !40 {
bb_1400075A0:
  %b1 = alloca ptr, align 8, !revenant.va !30
  %b2 = alloca ptr, align 8, !revenant.va !31
  #dbg_declare(ptr %b1, !41, !DIExpression(), !33)
  #dbg_declare(ptr %b2, !42, !DIExpression(), !33)
  store ptr %a1, ptr %b1, align 8, !revenant.va !34
  store ptr %a2, ptr %b2, align 8, !revenant.va !35
  ret ptr %a1, !revenant.va !36
}

; Caller: uses both callees, passing pointer values whose own type is the
; opaque pointer.
define i32 @sub_140008b57(ptr %p) #0 !dbg !50 {
bb_140008b57:
  %px = alloca ptr, align 8, !revenant.va !60
  #dbg_declare(ptr %px, !51, !DIExpression(), !52)
  store ptr %p, ptr %px, align 8, !revenant.va !53
  %63 = call ptr @sub_1400075A0(ptr %p, ptr %p), !revenant.va !54, !dbg !55
  %64 = call i32 @sub_1400077F0(ptr %63, ptr %p, i32 0), !revenant.va !56, !dbg !57
  ret i32 %64, !revenant.va !58
}

attributes #0 = { "revenant.va"="0" }

!llvm.dbg.cu = !{!2}
!llvm.module.flags = !{!0, !1}
!0 = !{i32 2, !"Debug Info Version", i32 3}
!1 = !{i32 4, !"revenant.lang", !"c"}
!2 = distinct !DICompileUnit(language: DW_LANG_C, file: !3, producer: "revenant", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug)
!3 = !DIFile(filename: "call-cast.exe", directory: "revenant")

; Types shared by the DISubprograms below.
!10 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!11 = !DIBasicType(name: "unsigned char", size: 8, encoding: DW_ATE_unsigned)
!12 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !11, size: 64)

; sub_1400077F0: i32 (rv_struct_sub_1400077F0_arg0 *, unsigned char *, int)
!20 = distinct !DISubprogram(name: "sub_1400077F0", linkageName: "sub_1400077F0", scope: !3, file: !3, line: 1, type: !23, scopeLine: 1, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !2)
!21 = !DILocation(line: 1, column: 1, scope: !20)
!22 = !DILocation(line: 1, column: 2, scope: !20)
!23 = !DISubroutineType(types: !24)
!24 = !{!10, !25, !12, !10}
!25 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !29, size: 64)
!29 = distinct !DICompositeType(tag: DW_TAG_structure_type, name: "rv_struct_sub_1400077F0_arg0", scope: !3, file: !3, line: 1, size: 64, elements: !39)
!39 = !{!38}
!38 = !DIDerivedType(tag: DW_TAG_member, name: "f", scope: !29, file: !3, line: 2, baseType: !10, size: 32)
!26 = !DILocalVariable(name: "a1", arg: 1, scope: !20, file: !3, line: 1, type: !25)
!27 = !DILocalVariable(name: "a2", arg: 2, scope: !20, file: !3, line: 1, type: !12)
!28 = !DILocalVariable(name: "a3", arg: 3, scope: !20, file: !3, line: 1, type: !10)

; sub_1400075A0: unsigned char *(unsigned char *, unsigned char *)
!40 = distinct !DISubprogram(name: "sub_1400075A0", linkageName: "sub_1400075A0", scope: !3, file: !3, line: 1, type: !43, scopeLine: 1, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !2)
!43 = !DISubroutineType(types: !44)
!44 = !{!12, !12, !12}
!41 = !DILocalVariable(name: "a1", arg: 1, scope: !40, file: !3, line: 1, type: !12)
!42 = !DILocalVariable(name: "a2", arg: 2, scope: !40, file: !3, line: 1, type: !12)
!33 = !DILocation(line: 1, column: 1, scope: !40)
!30 = !DILocation(line: 1, column: 1, scope: !40)
!31 = !DILocation(line: 1, column: 1, scope: !40)
!34 = !DILocation(line: 1, column: 1, scope: !40)
!35 = !DILocation(line: 1, column: 1, scope: !40)
!36 = !DILocation(line: 1, column: 1, scope: !40)

; sub_140008b57: i32 (unsigned char *)
!50 = distinct !DISubprogram(name: "sub_140008b57", linkageName: "sub_140008b57", scope: !3, file: !3, line: 1, type: !45, scopeLine: 1, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !2)
!45 = !DISubroutineType(types: !46)
!46 = !{!10, !12}
!51 = !DILocalVariable(name: "p", arg: 1, scope: !50, file: !3, line: 1, type: !12)
!52 = !DILocation(line: 1, column: 1, scope: !50)
!53 = !DILocation(line: 1, column: 1, scope: !50)
!54 = !DILocation(line: 2, column: 3, scope: !50)
!55 = !DILocation(line: 2, column: 3, scope: !50)
!56 = !DILocation(line: 3, column: 3, scope: !50)
!57 = !DILocation(line: 3, column: 3, scope: !50)
!58 = !DILocation(line: 4, column: 3, scope: !50)
!14 = !DILocation(line: 1, column: 1, scope: !20)
!15 = !DILocation(line: 1, column: 1, scope: !20)
!16 = !DILocation(line: 1, column: 1, scope: !20)
!17 = !DILocation(line: 1, column: 1, scope: !20)
!18 = !DILocation(line: 1, column: 1, scope: !20)
!19 = !DILocation(line: 1, column: 1, scope: !20)
!60 = !DILocation(line: 1, column: 1, scope: !50)
