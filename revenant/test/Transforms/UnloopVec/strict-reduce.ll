; Strict FP source-order lane reduction: eight consecutive loads feed eight
; chained fadds in order. Adapted from c-clang-o2 sum_vec_d. No fast-math
; flags are present; folding is valid only for this serial arithmetic chain.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unloopvec,verify -S %s | %FileCheck %s

define double @sum_ordered(ptr %a, i64 %n) {
entry:
  %positive = icmp ugt i64 %n, 0
  br i1 %positive, label %dispatch, label %exit

dispatch:
  %small = icmp ult i64 %n, 8
  br i1 %small, label %scalar.pre, label %vector.pre

vector.pre:
  %bound = and i64 %n, -8
  br label %vector

vector:
  %iv = phi i64 [ 0, %vector.pre ], [ %iv.next, %vector ]
  %acc = phi double [ 0.0, %vector.pre ], [ %sum7, %vector ]
  %p0 = getelementptr double, ptr %a, i64 %iv
  %v0 = load double, ptr %p0, align 8
  %sum0 = fadd double %acc, %v0
  %p1 = getelementptr double, ptr %p0, i64 1
  %v1 = load double, ptr %p1, align 8
  %sum1 = fadd double %sum0, %v1
  %p2 = getelementptr double, ptr %p0, i64 2
  %v2 = load double, ptr %p2, align 8
  %sum2 = fadd double %sum1, %v2
  %p3 = getelementptr double, ptr %p0, i64 3
  %v3 = load double, ptr %p3, align 8
  %sum3 = fadd double %sum2, %v3
  %p4 = getelementptr double, ptr %p0, i64 4
  %v4 = load double, ptr %p4, align 8
  %sum4 = fadd double %sum3, %v4
  %p5 = getelementptr double, ptr %p0, i64 5
  %v5 = load double, ptr %p5, align 8
  %sum5 = fadd double %sum4, %v5
  %p6 = getelementptr double, ptr %p0, i64 6
  %v6 = load double, ptr %p6, align 8
  %sum6 = fadd double %sum5, %v6
  %p7 = getelementptr double, ptr %p0, i64 7
  %v7 = load double, ptr %p7, align 8
  %sum7 = fadd double %sum6, %v7
  %iv.next = add i64 %iv, 8
  %vector.done = icmp eq i64 %iv.next, %bound
  br i1 %vector.done, label %middle, label %vector

middle:
  %all.done = icmp eq i64 %bound, %n
  br i1 %all.done, label %exit, label %scalar.pre

scalar.pre:
  %start = phi i64 [ 0, %dispatch ], [ %bound, %middle ]
  %seed = phi double [ 0.0, %dispatch ], [ %sum7, %middle ]
  br label %scalar

scalar:
  %i = phi i64 [ %start, %scalar.pre ], [ %i.next, %scalar ]
  %total = phi double [ %seed, %scalar.pre ], [ %total.next, %scalar ]
  %p = getelementptr double, ptr %a, i64 %i
  %v = load double, ptr %p, align 8
  %total.next = fadd double %total, %v
  %i.next = add i64 %i, 1
  %scalar.done = icmp eq i64 %i.next, %n
  br i1 %scalar.done, label %exit, label %scalar

exit:
  %result = phi double [ 0.0, %entry ], [ %sum7, %middle ], [ %total.next, %scalar ]
  ret double %result
}

; CHECK-LABEL: define double @sum_ordered
; CHECK-NOT: %sum7 = fadd
; CHECK: %total.next = fadd double %total, %v
; CHECK: }
