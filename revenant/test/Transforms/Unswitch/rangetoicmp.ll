; RUN: opt -load-pass-plugin=%shlibdir/RevenantTransformUnswitch.dll -passes=revenant-unswitch -S %s | FileCheck %s
; Range-to-icmp (SimplifyCFG turnSwitchRangeIntoICmp) back to a switch.
; Corpus: lua-clang-o1 lcorolib.pre.ll @luaB_close post shape
; (cases {1,2} folded to `add -1; icmp ult 2; br`).

define i32 @ranged(i32 %s) {
; CHECK-LABEL: define i32 @ranged(
; CHECK: switch i32 %s, label %other [
; CHECK-DAG: i32 1, label %dest
; CHECK-DAG: i32 2, label %dest
; CHECK: ]
entry:
  %sub = add i32 %s, -1, !revenant.va !{i64 4096}
  %rc = icmp ult i32 %sub, 2, !revenant.va !{i64 4097}
  br i1 %rc, label %dest, label %other

dest:
  ret i32 10

other:
  ret i32 -1
}
