; RUN: %revenant-opt -passes=revenant-reroll -S %s | %FileCheck %s
; Full unroll from corpus `arrays` (c-clang-o2 sub_140001330, pre-c-clang-o2.ll):
; 8 fully-unrolled `local[i] = i * idx` stores over byte-consecutive stack
; slots. Each lane is strength-reduced (mul idx,3 / shl idx,2 / ...), so no
; constant-shift synth can fire; the stored values are affine c*k + d in the
; window index over the shared leaf %t. Only the stores carry !revenant.va
; (monotonic, one shared range), mirroring real lifted IR where lane value
; defs and address GEPs lose provenance. Re-rolls to one counted loop body.

define i32 @arrays_full(ptr %frame, i32 %t) {
entry:
  %element237 = getelementptr inbounds nuw i32, ptr %frame, i64 10
  store i32 0, ptr %element237, align 1, !revenant.va !1
  %element238 = getelementptr inbounds nuw i32, ptr %frame, i64 11
  store i32 %t, ptr %element238, align 1, !revenant.va !3
  %t24 = shl i32 %t, 1
  %element239 = getelementptr inbounds nuw i32, ptr %frame, i64 12
  store i32 %t24, ptr %element239, align 1, !revenant.va !4
  %narrow181 = mul i32 %t, 3
  %element240 = getelementptr inbounds nuw i32, ptr %frame, i64 13
  store i32 %narrow181, ptr %element240, align 1, !revenant.va !5
  %narrow223 = shl i32 %t, 2
  %element241 = getelementptr inbounds nuw i32, ptr %frame, i64 14
  store i32 %narrow223, ptr %element241, align 1, !revenant.va !6
  %narrow183 = mul i32 %t, 5
  %element242 = getelementptr inbounds nuw i32, ptr %frame, i64 15
  store i32 %narrow183, ptr %element242, align 1, !revenant.va !7
  %narrow184 = mul i32 %t, 6
  %element243 = getelementptr inbounds nuw i32, ptr %frame, i64 16
  store i32 %narrow184, ptr %element243, align 1, !revenant.va !8
  %narrow185 = mul i32 %t, 7
  %element244 = getelementptr inbounds nuw i32, ptr %frame, i64 17
  store i32 %narrow185, ptr %element244, align 1, !revenant.va !9
  ret i32 %narrow185
}

; CHECK: define i32 @arrays_full
; A counted loop with an i64 trip counter appears.
; CHECK: unroll:
; CHECK: %u0i = phi i64 [ 0, %entry ], [ %u0inext, %unroll ]
; CHECK: %u0inext = add i64 %u0i, 1
; CHECK: %u0c = icmp ult i64 %u0inext, 8
; CHECK: br i1 %u0c, label %unroll, label %unroll.exit
; CHECK: unroll.exit:
; CHECK: ret i32

!0 = !{i64 5368714046}
!1 = !{i64 5368714051}
!2 = !{i64 5368714059}
!3 = !{i64 5368714066}
!4 = !{i64 5368714073}
!5 = !{i64 5368714084}
!6 = !{i64 5368714091}
!7 = !{i64 5368714098}
!8 = !{i64 5368714105}
!9 = !{i64 5368714112}
!10 = !{i64 5368714119}
