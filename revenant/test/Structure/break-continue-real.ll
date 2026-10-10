; RUN: %{rv-structure-tool} %s | %{FileCheck} %s

; Lifted from c-clang-o2 break_continue. Critical-edge blocks surround the
; induction update, continue predicate, break predicate, and common return.
define i32 @break_continue(i32 %n) {
bb_140001154:
  %v_r8.0.i32 = alloca i32
  %v_rax.2 = alloca i64
  %o = icmp slt i32 %n, 1
  br i1 %o, label %bb_140001154.bb_140001187_crit_edge, label %bb_140001154.bb_14000117a_crit_edge, !revenant.va !0
bb_140001160:
  %1 = load i32, ptr %v_r8.0.i32
  %2 = add i32 %1, 1
  %c = icmp eq i32 %n, %2
  br i1 %c, label %bb_140001160.bb_140001187_crit_edge, label %bb_140001160.bb_14000117a_crit_edge, !revenant.va !1
bb_14000117a:
  %3 = load i32, ptr %v_r8.0.i32
  %4 = urem i32 %3, 3
  %c100 = icmp eq i32 %4, 0
  br i1 %c100, label %bb_14000117a.bb_140001160_crit_edge, label %bb_140001187154, !revenant.va !2
bb_140001187154:
  %5 = load i32, ptr %v_r8.0.i32, !revenant.va !5
  %c116 = icmp ugt i32 %5, 20
  br i1 %c116, label %bb_140001187154.bb_140001187_crit_edge, label %bb_140001182, !revenant.va !3
bb_140001182:
  %6 = load i64, ptr %v_rax.2
  %t120 = trunc nuw i64 %6 to i32
  %7 = load i32, ptr %v_r8.0.i32
  %8 = add i32 %7, %t120
  %z134 = zext i32 %8 to i64
  store i64 %z134, ptr %v_rax.2
  br label %bb_140001160, !revenant.va !4
bb_140001187:
  %9 = load i64, ptr %v_rax.2, !revenant.va !5
  %e136 = trunc i64 %9 to i32
  ret i32 %e136, !revenant.va !5
bb_140001154.bb_140001187_crit_edge:
  store i64 0, ptr %v_rax.2
  br label %bb_140001187
bb_140001154.bb_14000117a_crit_edge:
  store i32 0, ptr %v_r8.0.i32
  store i64 0, ptr %v_rax.2
  br label %bb_14000117a
bb_140001160.bb_140001187_crit_edge:
  br label %bb_140001187
bb_140001160.bb_14000117a_crit_edge:
  store i32 %2, ptr %v_r8.0.i32
  br label %bb_14000117a
bb_14000117a.bb_140001160_crit_edge:
  br label %bb_140001160
bb_140001187154.bb_140001187_crit_edge:
  br label %bb_140001187
}
!0 = !{i64 5368713556}
!1 = !{i64 5368713568}
!2 = !{i64 5368713594}
!3 = !{i64 5368713600}
!4 = !{i64 5368713602}
!5 = !{i64 5368713607}
; CHECK-LABEL: @break_continue(
; CHECK: br i1 %{{.*}}, label %{{.*}}, label %{{.*}}, !revenant.region
