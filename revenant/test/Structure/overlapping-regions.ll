; RUN: %revenant-clang %s | %FileCheck %s
; REQUIRES: revenant-clang
;
; Reduced from spdlog-gcc-o0 @sub_140145C40: an "if" region whose member
; set overlaps a "while" region (the loop's members are post-dominated by
; the if's follow). Which region claims the shared blocks used to depend
; on DenseMap pointer-hash order, so runs alternated between a correct
; flat form and a structured form that dropped the if-header's statements
; (v_tmp1 was read uninitialized).
;
; Either emission shape (structured or degraded to gotos) must contain
; every block's statements: the stores feeding the folded conditions and
; the loop body may never drop.
;
; CHECK-LABEL: sub_1000(
; CHECK: t0 =
; CHECK: t1 =
; CHECK: if (
; CHECK: goto
; CHECK: while (
define i32 @sub_1000(ptr %a, i32 %n) {
bb_1000:
  %t0 = alloca i32, align 4
  %t1 = alloca i32, align 4
  %t2 = alloca i32, align 4
  %lv = alloca i32, align 4
  %v = load i32, ptr %a, align 4
  store i32 %v, ptr %t0, align 4
  %w = load i32, ptr %t0, align 4
  store i32 %w, ptr %t1, align 4
  %r = load i32, ptr %t1, align 4
  %m = and i32 %r, 1
  %c = icmp eq i32 %m, 0
  br i1 %c, label %bb_1010, label %bb_1020, !revenant.region !1

bb_1010:
  store i32 1, ptr %t2, align 4
  br label %bb_1030

bb_1020:
  store i32 2, ptr %t2, align 4
  br label %bb_1030

bb_1030:
  %h = load i32, ptr %t2, align 4
  %l = load i32, ptr %lv, align 4
  %cc = icmp slt i32 %l, %n
  br i1 %cc, label %bb_1040, label %bb_1060, !revenant.region !2

bb_1040:
  %x = load i32, ptr %t2, align 4
  %x2 = add i32 %x, 2
  store i32 %x2, ptr %t2, align 4
  br label %bb_1050

bb_1050:
  %k = load i32, ptr %lv, align 4
  %k2 = add i32 %k, 1
  store i32 %k2, ptr %lv, align 4
  br label %bb_1030

bb_1060:
  %res = load i32, ptr %t2, align 4
  ret i32 %res
}

!llvm.module.flags = !{!0}
!0 = !{i32 4, !"revenant.lang", !"c"}
; Block indices: entry=0, bb_1010=1, bb_1020=2, bb_1030=3, bb_1040=4,
; bb_1050=5, bb_1060=6.
!1 = !{!"if", i32 3}
!2 = !{!"while", i32 3, i32 5, i32 6}
