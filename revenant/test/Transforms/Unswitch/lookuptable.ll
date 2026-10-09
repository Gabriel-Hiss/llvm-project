; RUN: opt -load-pass-plugin=%shlibdir/RevenantTransformUnswitch.dll -passes=revenant-unswitch -S %s | FileCheck %s
; Lookup table (SimplifyCFG SwitchReplacement::LookupTableKind) back to a
; switch. Corpus: c-clang-o1..o3/oz main.post.ll @switch_twolevel
; (table [100 x5, 200 x5, 300 x5, 400 x5], default -1, range-checked).

@switch.table.switch_twolevel = private unnamed_addr constant [20 x i32] [i32 100, i32 100, i32 100, i32 100, i32 100, i32 200, i32 200, i32 200, i32 200, i32 200, i32 300, i32 300, i32 300, i32 300, i32 300, i32 400, i32 400, i32 400, i32 400, i32 400], align 4

define i32 @twolevel(i32 %0) {
; CHECK-LABEL: define i32 @twolevel(
; CHECK: switch i32 %0, label %[[DEF:.*]] [
; CHECK-DAG: i32 0, label %[[C:.*]]
; CHECK-DAG: i32 19, label %[[C]]
; CHECK: ]
; The fallback edge reaches the join directly, so its phi input must survive.
; CHECK: phi i32 [ %{{.*}}, %{{.*}} ], [ -1, %1 ],
  %2 = icmp ult i32 %0, 20, !revenant.va !{i64 4096}
  br i1 %2, label %3, label %7

3:
  %4 = zext i32 %0 to i64
  %5 = getelementptr i32, ptr @switch.table.switch_twolevel, i64 %4
  %6 = load i32, ptr %5, align 4
  br label %7

7:
  %8 = phi i32 [ %6, %3 ], [ -1, %1 ], !revenant.va !{i64 4097}
  ret i32 %8
}
