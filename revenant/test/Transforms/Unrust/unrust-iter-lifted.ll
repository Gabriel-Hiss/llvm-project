; Corpus: rust-o3 lifted ir-clean. Two spellings of the same loop. The
; demangler byte-scan loop (@sub_14000CCE4) keeps its frame as one big
; `alloca [N x i8]`, so the {ptr, len} iter struct is already inside it
; and no dead frame is added; the lifted range loop without a frame (the
; compiler post spelling, SROA'd) gets the 16-byte frame back. The
; `ult` compare must test the induction phi with a constant step: a bare
; `ult` on an argument (a `n <= 1` base case like tail_collatz) is not a
; range loop.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unrust -S %s | %FileCheck %s

!llvm.module.flags = !{!0}
!0 = !{i32 4, !"revenant.lang", !"rust"}

define i64 @framed_scan(ptr %s, i64 %len) {
; CHECK-LABEL: define i64 @framed_scan(
entry:
  %frame = alloca [984 x i8], align 1
  %fp33 = getelementptr inbounds nuw i8, ptr %frame, i64 40
  store i64 %len, ptr %fp33, align 1
  br label %loop

loop:                                              ; preds = %body, %entry
  %i = phi i64 [ 0, %entry ], [ %next, %body ]
  %c = icmp ult i64 %i, %len, !revenant.va !{i64 14938}
  br i1 %c, label %body, label %exit

body:                                              ; preds = %loop
  %e = getelementptr i8, ptr %s, i64 %i
  %v = load i8, ptr %e, align 1
  %next = add i64 %i, 1
  br label %loop

exit:                                              ; preds = %loop
  ret i64 0
; The frame is already there: no dead [16 x i8] duplicate.
; CHECK-NOT: iter.unrust
}

define i64 @sroa_scan(ptr %s, i64 %len) {
; CHECK-LABEL: define i64 @sroa_scan(
entry:
  br label %loop

loop:                                              ; preds = %body, %entry
  %i = phi i64 [ 0, %entry ], [ %next, %body ]
  %c = icmp ult i64 %i, %len, !revenant.va !{i64 14938}
  br i1 %c, label %body, label %exit

body:                                              ; preds = %loop
  %e = getelementptr i8, ptr %s, i64 %i
  %v = load i8, ptr %e, align 1
  %next = add i64 %i, 1
  br label %loop

exit:                                              ; preds = %loop
  ret i64 0
; CHECK: %iter.unrust = alloca [16 x i8], align 8
}

define i64 @base_case(i64 %n) {
; CHECK-LABEL: define i64 @base_case(
entry:
  %c = icmp ult i64 %n, 2, !revenant.va !{i64 741}
  br i1 %c, label %done, label %step

step:                                              ; preds = %step, %entry
  %k = phi i64 [ %n, %entry ], [ %next, %step ]
  %next = lshr i64 %k, 1
  %c2 = icmp ugt i64 %next, 1
  br i1 %c2, label %step, label %done

done:                                              ; preds = %step, %entry
  ret i64 0
; No range loop: the ult tests an argument and the step is a shift.
; CHECK-NOT: iter.unrust
}
