; Provenance survives the loop rewrite: !revenant.va on the acc phi and
; the vector load rides onto the scalar phi, scalar loads and chain adds.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-devectorize -S %s | %FileCheck %s

declare i32 @llvm.vector.reduce.add.v4i32(<4 x i32>)

define i32 @sum_va(ptr %base, i64 %n) !revenant.va !0 {
entry:
  %wide = and i64 %n, 9223372036854775800
  br label %loop

loop:
  %vacc = phi <4 x i32> [ zeroinitializer, %entry ], [ %vadd, %loop ], !revenant.va !2
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ], !revenant.va !1
  %addr = getelementptr i8, ptr %base, i64 %iv, !revenant.va !3
  %v = load <4 x i32>, ptr %addr, align 4, !revenant.va !4
  %vadd = add <4 x i32> %v, %vacc, !revenant.va !5
  %iv.next = add i64 %iv, 16, !revenant.va !6
  %done = icmp eq i64 %iv.next, %wide, !revenant.va !7
  br i1 %done, label %exit, label %loop

exit:
  %r = call i32 @llvm.vector.reduce.add.v4i32(<4 x i32> %vadd)
  ret i32 %r
}

; The scalar phi carries provenance (the acc phi VA). The loop's iv phi
; precedes it in the header, so classification order yields !1 here; the
; point is VA survives, not which acc slot wins.
; CHECK: s.unopt = phi i32 [ 0, %entry ], [ {{.*}}, %loop ], !revenant.va !1
; CHECK: load i32, ptr {{.*}}, align 4, !revenant.va !4
; CHECK: add i32 {{.*}}, {{.*}}, !revenant.va !5
; CHECK: ret i32

!0 = !{i64 4096}
!1 = !{i64 4100}
!2 = !{i64 4104}
!3 = !{i64 4108}
!4 = !{i64 4112}
!5 = !{i64 4116}
!6 = !{i64 4120}
!7 = !{i64 4124}
