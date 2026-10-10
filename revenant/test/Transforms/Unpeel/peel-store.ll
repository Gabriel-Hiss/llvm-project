; RUN: %revenant-opt -passes=revenant-unpeel -S %s | %FileCheck %s
; Peel with store: block %P holds a peeled first iteration that stores
; to the original pointer. The loop's first store uses a phi that takes
; P's pointer as init. After the fold, P is deleted and the loop's phi
; takes G's pointer directly.
; CHECK-LABEL: define void @ps(

define void @ps(ptr %dst, i32 %v, i32 %n) {
G:
  %c0 = icmp sgt i32 %n, 0, !revenant.va !{i64 4096}
  br i1 %c0, label %P, label %X
P:
  store i32 %v, ptr %dst, align 1, !revenant.va !{i64 4112}
  br label %H
H:
  %p = phi ptr [ %dst, %P ], [ %pn, %H ]
  %i = phi i32 [ 1, %P ], [ %in, %H ]
  store i32 %v, ptr %p, align 1, !revenant.va !{i64 4128}
  %pn = getelementptr i32, ptr %p, i32 1, !revenant.va !{i64 4129}
  %in = add i32 %i, 1, !revenant.va !{i64 4130}
  %c = icmp slt i32 %in, %n, !revenant.va !{i64 4131}
  br i1 %c, label %H, label %X
; CHECK-NOT: P:
; CHECK: br i1 %c0, label %H, label %X
X:
  ret void, !revenant.va !{i64 4132}
}
