; RUN: opt -load-pass-plugin=%shlibdir/RevenantTransformUniv.dll -passes=revenant-univ,verify -S %s | FileCheck %s

; Shape from target/parity/c-clang-o2/artifacts/main.post.ll @sum_vec.
; The scalar tail uses a widened i64 IV, equality exit, and typed i32 GEP.
define i32 @sum_vec(ptr %base, i32 %n) {
entry:
  %positive = icmp sgt i32 %n, 0
  br i1 %positive, label %pre, label %exit
pre:
  %wide = zext i32 %n to i64
  br label %loop
loop:
  %iv = phi i64 [ 0, %pre ], [ %next, %loop ]
  %sum = phi i32 [ 0, %pre ], [ %sum.next, %loop ]
  %addr = getelementptr inbounds i32, ptr %base, i64 %iv, !revenant.va !0
  %value = load i32, ptr %addr, align 4
  %sum.next = add i32 %sum, %value
  %next = add i64 %iv, 1
  %done = icmp eq i64 %next, %wide
  br i1 %done, label %exit, label %loop
exit:
  %result = phi i32 [ 0, %entry ], [ %sum.next, %loop ]
  ret i32 %result
}

; CHECK-LABEL: define i32 @sum_vec(
; CHECK: %iv.idx = phi i32 [ 0, %pre ], [ %next.idx, %loop ]
; CHECK: %addr.idx = getelementptr inbounds i32, ptr %base, i32 %iv.idx, !revenant.va !0
; CHECK: %next.idx = add i32 %iv.idx, 1
; CHECK: %done.idx = icmp slt i32 %next.idx, %n
; CHECK: br i1 %done.idx, label %loop, label %exit

!0 = !{i64 4096}

; Shape from lifted c-clang-o2 `fill_vec` (VA 0x1400017F0): the loop header
; reconstructs its base pointer with inttoptr from an invariant integer arg.
define void @fill_vec(i64 %base.bits, i32 %value, i32 %n) {
entry:
  %wide = zext i32 %n to i64
  %empty = icmp slt i32 %n, 1
  br i1 %empty, label %exit, label %pre
pre:
  %small = icmp ult i32 %n, 8
  call void @llvm.assume(i1 %small)
  br label %loop
loop:
  %iv = phi i64 [ 0, %pre ], [ %next, %loop ]
  %base = inttoptr i64 %base.bits to ptr
  %addr = getelementptr i32, ptr %base, i64 %iv, !revenant.va !1
  store i32 %value, ptr %addr, align 1
  %next = add i64 %iv, 1
  %done = icmp eq i64 %next, %wide
  br i1 %done, label %exit, label %loop
exit:
  ret void
}

; CHECK-LABEL: define void @fill_vec(
; CHECK: %iv.idx = phi i32 [ 0, %pre ], [ %next.idx, %loop ]
; CHECK: %addr.idx = getelementptr inbounds i32, ptr %base, i32 %iv.idx, !revenant.va !1
; CHECK: %next.idx = add i32 %iv.idx, 1
; CHECK: %done.idx = icmp slt i32 %next.idx, %n
; CHECK: br i1 %done.idx, label %loop, label %exit

; Minimal reproduction of the independent-base address pair in lifted
; c-clang-o2 `copy_vec`; source and destination share one integer IV.
define void @copy_vec(ptr %dst, ptr %src, i32 %n) {
entry:
  %wide = zext i32 %n to i64
  br label %loop
loop:
  %iv = phi i64 [ 0, %entry ], [ %next, %loop ]
  %src.addr = getelementptr i32, ptr %src, i64 %iv, !revenant.va !0
  %value = load i32, ptr %src.addr, align 4
  %dst.addr = getelementptr i32, ptr %dst, i64 %iv, !revenant.va !0
  store i32 %value, ptr %dst.addr, align 4
  %next = add i64 %iv, 1
  %done = icmp eq i64 %next, %wide
  br i1 %done, label %exit, label %loop
exit:
  ret void
}

; CHECK-LABEL: define void @copy_vec(
; CHECK: %iv.idx = phi i32 [ 0, %entry ], [ %next.idx, %loop ]
; CHECK: %src.addr.idx = getelementptr inbounds i32, ptr %src, i32 %iv.idx, !revenant.va !0
; CHECK: %dst.addr.idx = getelementptr inbounds i32, ptr %dst, i32 %iv.idx, !revenant.va !0
; CHECK: %next.idx = add i32 %iv.idx, 1
; CHECK: %done.idx = icmp slt i32 %next.idx, %n
; CHECK: br i1 %done.idx, label %loop, label %exit

declare void @llvm.assume(i1)
!1 = !{i64 8192}
