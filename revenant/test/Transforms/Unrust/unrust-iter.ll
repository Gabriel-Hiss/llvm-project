; Corpus: rust-o3 artifacts/corpus_rust.pre.ll @sum_slice_i32 keeps the
; `{ptr, len}` slice-iter allocas with an `ult` latch compare; post keeps
; only scalar phis and dbg_value fragments. The pass rebuilds a 16-byte
; iter frame when an ult-guarded loop branch survives without one.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unrust -S %s | %FileCheck %s

!llvm.module.flags = !{!0}
!0 = !{i32 4, !"revenant.lang", !"rust"}

define i32 @iterloop(ptr %p, i64 %n) {
; CHECK-LABEL: define i32 @iterloop(
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %next, %latch ]
  %c = icmp ult i64 %i, %n, !revenant.va !{i64 4096}
  br i1 %c, label %body, label %exit
body:
  %v = load i64, ptr %p, align 8
  br label %latch
latch:
  %next = add i64 %i, 1
  br label %loop
exit:
  %s = phi i64 [ 0, %loop ]
  %t = trunc i64 %s to i32
  ret i32 %t
; CHECK: %iter.unrust = alloca [16 x i8], align 8
}
