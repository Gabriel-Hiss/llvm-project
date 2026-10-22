; Minimal real IR from c-gcc-o2 main sub_140003B00 (board line 421).
; The while tag has a preheader IV init, incremented header comparison, and
; latch store; recover it as a for loop without losing the tested value.
; RUN: python %S/loop-latch-backedge-run.py %s sub_140003B00 3
; RUN: %revenant-clang %s --lang=c | %FileCheck %s --check-prefix=SHAPE
; REQUIRES: structured-source-run,revenant-clang

source_filename = "repro.exe"
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

!llvm.module.flags = !{!8}
!8 = !{i32 4, !"revenant.lang", !"c"}

; Block order: entry=0, header=1, latch=2, exit=3.
define i32 @sub_140003B00() {
bb_140003e0b:
  %a = alloca i64, align 8, !revenant.va !0
  %sum = alloca i32, align 4
  store i32 0, ptr %sum, align 4
  store i64 0, ptr %a, align 8, !revenant.va !0
  br label %bb_head

bb_head:
  %iv = load i64, ptr %a, align 8, !revenant.va !1
  %next = add i64 %iv, 16, !revenant.va !4
  %done = icmp eq i64 %next, 64, !revenant.va !5
  br i1 %done, label %bb_exit, label %bb_latch, !revenant.region !6

bb_latch:
  %total = load i32, ptr %sum, align 4
  %total.next = add i32 %total, 1
  store i32 %total.next, ptr %sum, align 4
  store i64 %next, ptr %a, align 8, !revenant.va !7
  br label %bb_head

bb_exit:
  %result = load i32, ptr %sum, align 4
  ret i32 %result
}

!0 = !{i64 5368724896}
!1 = !{i64 5368724655}
!4 = !{i64 5368724681}
!5 = !{i64 5368724689}
!6 = !{!"while", i32 1, i32 2, i32 3}
!7 = !{i64 5368724700}

; SHAPE-LABEL: sub_140003B00(
; SHAPE: for (
