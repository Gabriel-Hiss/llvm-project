; Corpus: rust-o3 lifted ir-clean. The sigs recover most panic callees
; by name, but a callee they cannot name stays a `sub_<VA>` declaration;
; the dedicated panic block (one call plus `unreachable`, no other
; effect) is the panic_bounds_check path's shape. The same flipped
; `icmp ugt len, idx` guard must still flip back to `icmp ult idx, len`.
; The `revenant.asm.jmp` tail also ends in unreachable but carries the
; opaque marker and is never a panic block.
; RUN: %revenant-opt -passes=revenant-unrust -S %s | %FileCheck %s

!llvm.module.flags = !{!0}
!0 = !{i32 4, !"revenant.lang", !"rust"}

declare void @sub_140017310(i64)
declare void @revenant.asm.jmp(...)

define void @sub_14000AAA0_lifted(ptr %hay, i64 %len, i64 %idx) {
; CHECK-LABEL: define void @sub_14000AAA0_lifted(
entry:
  %c = icmp ugt i64 %len, %idx, !revenant.va !{i64 700}
  br i1 %c, label %bb_ok, label %bb_fail, !revenant.va !{i64 701}

bb_fail:                                          ; preds = %entry
  call void @sub_140017310(i64 ptrtoint (ptr @g_140018a50 to i64)), !revenant.va !{i64 702}
  unreachable

bb_ok:                                            ; preds = %entry
  %e = getelementptr i8, ptr %hay, i64 %idx
  %v = load i8, ptr %e, align 1
  ret void
; CHECK: icmp ult i64 %idx, %len
; CHECK-NOT: icmp ugt
}

@g_140018a50 = internal constant [16 x i8] zeroinitializer
