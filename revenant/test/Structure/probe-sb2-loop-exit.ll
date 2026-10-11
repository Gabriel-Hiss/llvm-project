; Real minimal counterexample from StepReviewEmit: a region-header multi-use
; IV step is used by the first loop test and by the latch store. The entry
; initializes i=0 and lim=1, so the first test exits with marker=0; executing
; the latch once instead returns marker=42. Compile emitted C and compare main
; with lli to catch both a missing first-test seed and the wrong loop exit.
; RUN: python %S/loop-latch-backedge-run.py %s --entry
; REQUIRES: structured-source-run

source_filename = "probe-sb2.exe"
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

define i32 @sub_loop(i64 %n) #0 {
entry:
  %acc = alloca i64, align 8, !revenant.va !4
  %i = alloca i64, align 8, !revenant.va !5
  %lim = alloca i64, align 8, !revenant.va !6
  %marker = alloca i64, align 8, !revenant.va !60
  store i64 0, ptr %acc, align 8, !revenant.va !7
  store i64 0, ptr %i, align 8, !revenant.va !8
  store i64 %n, ptr %lim, align 8, !revenant.va !9
  store i64 0, ptr %marker, align 8, !revenant.va !61
  br label %loop, !revenant.va !10

loop:
  %iv = load i64, ptr %i, align 8, !revenant.va !11
  %a = load i64, ptr %acc, align 8, !revenant.va !12
  %limv = load i64, ptr %lim, align 8, !revenant.va !13
  %step = add i64 %iv, 1, !revenant.va !14
  %again = icmp eq i64 %limv, %step, !revenant.va !15
  br i1 %again, label %exit, label %latch, !revenant.va !16, !revenant.region !20

latch:
  %next = add i64 %a, %iv, !revenant.va !17
  store i64 %next, ptr %acc, align 8, !revenant.va !18
  store i64 %step, ptr %i, align 8, !revenant.va !19
  store i64 42, ptr %marker, align 8, !revenant.va !62
  br label %loop, !revenant.va !21

exit:
  %m = load i64, ptr %marker, align 8, !revenant.va !63
  %o = trunc i64 %m to i32, !revenant.va !23
  ret i32 %o, !revenant.va !24
}

define i32 @ifhead(i64 %x) #0 {
entry:
  %s = alloca i64, align 8, !revenant.va !30
  store i64 0, ptr %s, align 8, !revenant.va !31
  br label %test, !revenant.va !32

test:
  %v = load i64, ptr %s, align 8, !revenant.va !33
  %c = icmp eq i64 %v, %x, !revenant.va !34
  br i1 %c, label %then, label %join, !revenant.va !35

then:
  %w = add i64 %v, 1, !revenant.va !36
  store i64 %w, ptr %s, align 8, !revenant.va !37
  br label %join, !revenant.va !38

join:
  %r = load i64, ptr %s, align 8, !revenant.va !39
  %o = trunc i64 %r to i32, !revenant.va !40
  ret i32 %o, !revenant.va !41
}

define i32 @main(i32 %argc, ptr %argv) #0 {
bb_140001000:
  %l = call i32 @sub_loop(i64 1), !revenant.va !50
  %i0 = call i32 @ifhead(i64 7), !revenant.va !51
  %i1 = call i32 @ifhead(i64 0), !revenant.va !52
  %s2 = mul i32 %i0, 10, !revenant.va !54
  %s3 = mul i32 %i1, 100, !revenant.va !55
  %t1 = add i32 %l, %s2, !revenant.va !56
  %t2 = add i32 %t1, %s3, !revenant.va !57
  ret i32 %t2, !revenant.va !58
}

attributes #0 = { "revenant.va"="0" }
!llvm.module.flags = !{!0}
!0 = !{i32 4, !"revenant.lang", !"c"}
!4 = !{i64 5368724480}
!5 = !{i64 5368724484}
!6 = !{i64 5368724488}
!7 = !{i64 5368724492}
!8 = !{i64 5368724496}
!9 = !{i64 5368724500}
!10 = !{i64 5368724504}
!11 = !{i64 5368724512}
!12 = !{i64 5368724516}
!13 = !{i64 5368724520}
!14 = !{i64 5368724524}
!15 = !{i64 5368724528}
!16 = !{i64 5368724532}
!17 = !{i64 5368724544}
!18 = !{i64 5368724548}
!19 = !{i64 5368724552}
!20 = !{!"while", i32 1, i32 2, i32 3}
!21 = !{i64 5368724556}
!23 = !{i64 5368724564}
!24 = !{i64 5368724568}
!30 = !{i64 5368725248}
!31 = !{i64 5368725252}
!32 = !{i64 5368725256}
!33 = !{i64 5368725264}
!34 = !{i64 5368725268}
!35 = !{i64 5368725272}
!36 = !{i64 5368725280}
!37 = !{i64 5368725284}
!38 = !{i64 5368725288}
!39 = !{i64 5368725296}
!40 = !{i64 5368725300}
!41 = !{i64 5368725304}
!50 = !{i64 5368725504}
!51 = !{i64 5368725508}
!52 = !{i64 5368725512}
!54 = !{i64 5368725520}
!55 = !{i64 5368725524}
!56 = !{i64 5368725528}
!57 = !{i64 5368725532}
!58 = !{i64 5368725536}
!60 = !{i64 5368724576}
!61 = !{i64 5368724580}
!62 = !{i64 5368724584}
!63 = !{i64 5368724588}
