; RUN: python %S/loop-latch-backedge-run.py %s sub_140001210 10 27
; REQUIRES: structured-source-run
;
; Real lifted c-o0 break_continue at 0x140001210. For n=10 it returns 27;
; The test runner adds main and compares the emitted C executable with lli.

target triple = "x86_64-pc-windows-msvc"

define i32 @sub_140001210(i32 %0) #0 !dbg !4 {
bb_140001227:
  %v_frame.sroa.0.0 = alloca i32, align 4
    #dbg_declare(ptr %v_frame.sroa.0.0, !9, !DIExpression(), !11)
  %v_frame.sroa.4.0 = alloca i32, align 4
    #dbg_value(i32 %0, !12, !DIExpression(), !13)
    #dbg_declare(ptr %v_frame.sroa.4.0, !14, !DIExpression(), !11)
  store i32 0, ptr %v_frame.sroa.4.0, align 4, !revenant.va !15
  store i32 0, ptr %v_frame.sroa.0.0, align 4, !revenant.va !15
  br label %bb_14000126e, !revenant.va !15

bb_14000122c:                                     ; preds = %bb_x0, %bb_14000125c
  %1 = load i32, ptr %v_frame.sroa.0.0, align 4, !revenant.va !16
  %2 = add i32 %1, 1, !revenant.va !16
  store i32 %2, ptr %v_frame.sroa.0.0, align 4, !revenant.va !17
  br label %bb_14000126e, !revenant.va !17

bb_14000126e:                                     ; preds = %bb_14000122c, %bb_140001227
  %3 = load i32, ptr %v_frame.sroa.0.0, align 4, !revenant.va !18
  %c.not = icmp slt i32 %3, %0, !revenant.va !18
  br i1 %c.not, label %bb_140001243, label %bb_14000126e157, !revenant.va !19, !revenant.region !20

bb_140001243:                                     ; preds = %bb_14000126e
  %4 = load i32, ptr %v_frame.sroa.0.0, align 4
  %5 = sext i32 %4 to i64
  %6 = srem i64 %5, 3, !revenant.va !21
  %c78.not = icmp eq i64 %6, 0
  br i1 %c78.not, label %bb_x0, label %bb_140001251, !revenant.va !22, !revenant.region !23

bb_140001251:                                     ; preds = %bb_140001243
  %7 = load i32, ptr %v_frame.sroa.0.0, align 4
  %c93 = icmp slt i32 %7, 21
  br i1 %c93, label %bb_14000125c, label %bb_14000126e157, !revenant.va !24, !revenant.region !23

bb_14000125c:                                     ; preds = %bb_140001251
  %8 = load i32, ptr %v_frame.sroa.4.0, align 4, !revenant.va !25
  %9 = load i32, ptr %v_frame.sroa.0.0, align 4, !revenant.va !25
  %10 = add i32 %8, %9, !revenant.va !25
  store i32 %10, ptr %v_frame.sroa.4.0, align 4, !revenant.va !26
  br label %bb_14000122c, !revenant.va !26

bb_14000126e157:                                  ; preds = %bb_140001251, %bb_14000126e
  %11 = load i32, ptr %v_frame.sroa.4.0, align 4, !revenant.va !18
  ret i32 %11, !revenant.va !18

bb_x0:                                            ; preds = %bb_140001243
  br label %bb_14000122c
}


declare i8 @llvm.ctpop.i8(i8) #1

attributes #0 = { "revenant.user"="true" "revenant.va"="0x140001210" }
attributes #1 = { nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none) }

!llvm.module.flags = !{!0, !1}
!llvm.dbg.cu = !{!2}

!0 = !{i32 4, !"revenant.lang", !"c"}
!1 = !{i32 2, !"Debug Info Version", i32 3}
!2 = distinct !DICompileUnit(language: DW_LANG_C, file: !3, producer: "revenant", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug)
!3 = !DIFile(filename: "revenant.ll", directory: "revenant")
!4 = distinct !DISubprogram(name: "sub_140001210", linkageName: "sub_140001210", scope: !3, file: !3, line: 1, type: !5, scopeLine: 1, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !2, retainedNodes: !8)
!5 = !DISubroutineType(types: !6)
!6 = !{!7, !7}
!7 = !DIBasicType(name: "int32_t", size: 32, encoding: DW_ATE_signed)
!8 = !{}
!9 = !DILocalVariable(name: "v_frame.sroa.0.0", scope: !4, file: !3, line: 1, type: !10)
!10 = !DIBasicType(name: "long", size: 32, encoding: DW_ATE_signed)
!11 = !DILocation(line: 1, scope: !4)
!12 = !DILocalVariable(name: "a0", arg: 1, scope: !4, file: !3, line: 1, type: !7)
!13 = !DILocation(line: 1, column: 1, scope: !4)
!14 = !DILocalVariable(name: "v_frame.sroa.4.0", scope: !4, file: !3, line: 1, type: !10)
!15 = !{i64 5368713767}
!16 = !{i64 5368713772}
!17 = !{i64 5368713774}
!18 = !{i64 5368713838}
!19 = !{i64 5368713784}
!20 = !{!"while", i32 2, i32 1, i32 6}
!21 = !{i64 5368713795}
!22 = !{i64 5368713801}
!23 = !{!"if", i32 6}
!24 = !{i64 5368713809}
!25 = !{i64 5368713820}
!26 = !{i64 5368713828}
