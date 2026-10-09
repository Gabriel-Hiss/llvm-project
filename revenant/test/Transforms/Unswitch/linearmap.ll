; RUN: %revenant-opt -passes=revenant-unswitch -S %s | %FileCheck %s
; Linear map (SimplifyCFG SwitchReplacement::LinearMapKind) back to a
; switch. Corpus: c-clang-o1..o3/oz main.post.ll @switch_offset
; (result = 30 + 10*i for i in [0,10), default -1).

define i32 @offset(i32 %0) {
; CHECK-LABEL: define i32 @offset(
; CHECK: switch i32 %0, label %{{.*}} [
; CHECK-DAG: i32 3, label %{{.*}}
; CHECK-DAG: i32 12, label %{{.*}}
; CHECK: ]
  %2 = add i32 %0, -3, !revenant.va !{i64 4096}
  %3 = icmp ult i32 %2, 10, !revenant.va !{i64 4097}
  %4 = mul i32 %2, 10
  %5 = add i32 %4, 30
  %6 = select i1 %3, i32 %5, i32 -1, !revenant.va !{i64 4098}
  ret i32 %6
}
