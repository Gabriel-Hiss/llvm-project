; Reduced real ir-final c-clang-o2lto sub_1400010C0 (board 497): the loop
; has a generic loop header, nested exit tests, and a two-store latch. A main
; wrapper ignores the opaque return so lli/native success means the loop exits.
; RUN: python %S/loop-latch-backedge-run.py %s --entry
; REQUIRES: structured-source-run

target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

!llvm.module.flags = !{!0}
!0 = !{i32 4, !"revenant.lang", !"c"}
!1 = !{!"if", i32 8}
!2 = !{!"loop", i32 2, i32 9, i32 8}
!3 = !{!2, !1}

define float @sub_1400010C0() {
bb_1400010cf:
  %v_rcx = alloca i64, align 8
  %v_r8 = alloca i32, align 4
  %v_rax = alloca i64, align 8
  store i32 0, ptr %v_r8, align 4
  store i64 1, ptr %v_rcx, align 8
  store i64 0, ptr %v_rax, align 8
  br label %bb_1400010f4

bb_1400010e0:
  %0 = load i32, ptr %v_r8, align 4
  %1 = add i32 %0, 2
  %2 = add i64 %4, 2
  %c = icmp eq i32 %1, 30
  br i1 %c, label %bb_14000112e, label %bb_x0, !revenant.region !1

bb_1400010f4:
  %3 = load i64, ptr %v_rcx, align 8
  %4 = and i64 %3, 4294967295
  %5 = load i32, ptr %v_r8, align 4
  %6 = zext i32 %5 to i64
  %7 = mul i64 %6, 2863311531
  %8 = lshr i64 %7, 33
  %9 = trunc i64 %8 to i32
  %narrow = mul i32 %9, 3
  %10 = load i32, ptr %v_r8, align 4
  %c99 = icmp eq i32 %narrow, %10
  br i1 %c99, label %bb_x1, label %bb_14000112e265, !revenant.region !3

bb_14000112e265:
  %11 = load i32, ptr %v_r8, align 4
  %c115 = icmp ugt i32 %11, 20
  br i1 %c115, label %bb_14000112e, label %bb_140001107, !revenant.region !1

bb_140001107:
  %12 = load i64, ptr %v_rax, align 8
  %t119 = trunc i64 %12 to i32
  %13 = load i32, ptr %v_r8, align 4
  %14 = add i32 %13, %t119
  %z133 = zext i32 %14 to i64
  store i64 %z133, ptr %v_rax, align 8
  br label %bb_140001111

bb_140001111:
  %narrow264 = mul i64 %4, 2863311531
  %15 = lshr i64 %narrow264, 33
  %16 = trunc i64 %15 to i32
  %narrow235 = mul i32 %16, 3
  %17 = add i32 %narrow235, -1
  %18 = load i32, ptr %v_r8, align 4
  %c183 = icmp eq i32 %17, %18
  br i1 %c183, label %bb_x2, label %bb_14000112e266, !revenant.region !1

bb_14000112e266:
  %19 = load i32, ptr %v_r8, align 4
  %c199 = icmp ugt i32 %19, 19
  br i1 %c199, label %bb_14000112e, label %bb_140001127, !revenant.region !1

bb_140001127:
  %20 = load i64, ptr %v_rax, align 8
  %t203 = trunc i64 %20 to i32
  %21 = load i32, ptr %v_r8, align 4
  %22 = add i32 %21, %t203
  %23 = add i32 %22, 1
  %z234 = zext i32 %23 to i64
  store i64 %z234, ptr %v_rax, align 8
  br label %bb_1400010e0

bb_14000112e:
  ret float undef

bb_x0:
  store i32 %1, ptr %v_r8, align 4
  store i64 %2, ptr %v_rcx, align 8
  br label %bb_1400010f4

bb_x1:
  br label %bb_140001111

bb_x2:
  br label %bb_1400010e0
}

define i32 @main(i32 %argc, ptr %argv) {
entry:
  %ignored = call float @sub_1400010C0()
  ret i32 0
}
