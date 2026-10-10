; RUN: %revenant-opt -passes=revenant-reroll,verify -S %s | %FileCheck %s

; Fixture: multi-accumulator unrolled loop (gcc -O3 / clang interleaving).
; Two accumulator PHIs each fed per window, merged after the loop.
; The fold merges them into one accumulator and one body copy.

define i32 @sum2_acc(ptr %a, i32 %n) {
entry:
  %n2 = zext i32 %n to i64
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %acc0 = phi i32 [ 0, %entry ], [ %acc0.next, %loop ]
  %acc1 = phi i32 [ 0, %entry ], [ %acc1.next, %loop ]
  %addr0 = getelementptr i32, ptr %a, i64 %i, !revenant.va !1
  %x = load i32, ptr %addr0, align 1, !revenant.va !2
  %acc0.next = add i32 %acc0, %x, !revenant.va !3
  %addr1 = getelementptr i32, ptr %addr0, i64 1, !revenant.va !4
  %y = load i32, ptr %addr1, align 1, !revenant.va !5
  %acc1.next = add i32 %acc1, %y, !revenant.va !6
  %next = add i64 %i, 2, !revenant.va !7
  %done = icmp eq i64 %next, %n2, !revenant.va !8
  br i1 %done, label %exit, label %loop, !revenant.va !9

exit:
  %s = add i32 %acc0, %acc1, !revenant.va !10
  ret i32 %s, !revenant.va !11
}

; CHECK-LABEL: define i32 @sum2_acc(
; CHECK: %ua_merge = phi i32 [ 0, %entry ], [ %ua_upd1, %loop ]
; CHECK: %next = add i64 %i, 2
; CHECK-NOT: %acc0.next
; CHECK-NOT: %acc1.next

; --- Case 2: three accumulators ---

define i32 @sum3_acc(ptr %a, i32 %n) {
entry:
  %n3 = zext i32 %n to i64
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %acc0 = phi i32 [ 0, %entry ], [ %acc0.next, %loop ]
  %acc1 = phi i32 [ 0, %entry ], [ %acc1.next, %loop ]
  %acc2 = phi i32 [ 0, %entry ], [ %acc2.next, %loop ]
  %addr0 = getelementptr i32, ptr %a, i64 %i, !revenant.va !12
  %x = load i32, ptr %addr0, align 1, !revenant.va !13
  %acc0.next = add i32 %acc0, %x, !revenant.va !14
  %addr1 = getelementptr i32, ptr %addr0, i64 1, !revenant.va !15
  %y = load i32, ptr %addr1, align 1, !revenant.va !16
  %acc1.next = add i32 %acc1, %y, !revenant.va !17
  %addr2 = getelementptr i32, ptr %addr0, i64 2, !revenant.va !18
  %z = load i32, ptr %addr2, align 1, !revenant.va !19
  %acc2.next = add i32 %acc2, %z, !revenant.va !20
  %next = add i64 %i, 3, !revenant.va !21
  %done = icmp eq i64 %next, %n3, !revenant.va !22
  br i1 %done, label %exit, label %loop, !revenant.va !23

exit:
  %s01 = add i32 %acc0, %acc1, !revenant.va !24
  %s = add i32 %s01, %acc2, !revenant.va !25
  ret i32 %s, !revenant.va !26
}

; CHECK-LABEL: define i32 @sum3_acc(
; CHECK-NOT: ua_merge
; CHECK-NOT: ua_upd

; --- Case 3: dot product style ---

define i32 @dot_acc(ptr %a, ptr %b, i32 %n) {
entry:
  %n2 = zext i32 %n to i64
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %acc0 = phi i32 [ 0, %entry ], [ %acc0.next, %loop ]
  %acc1 = phi i32 [ 0, %entry ], [ %acc1.next, %loop ]
  %a0 = getelementptr i32, ptr %a, i64 %i, !revenant.va !27
  %x = load i32, ptr %a0, align 1, !revenant.va !28
  %b0 = getelementptr i32, ptr %b, i64 %i, !revenant.va !29
  %y = load i32, ptr %b0, align 1, !revenant.va !30
  %p0 = mul i32 %x, %y, !revenant.va !31
  %acc0.next = add i32 %acc0, %p0, !revenant.va !32
  %a1 = getelementptr i32, ptr %a0, i64 1, !revenant.va !33
  %x1 = load i32, ptr %a1, align 1, !revenant.va !34
  %b1 = getelementptr i32, ptr %b0, i64 1, !revenant.va !35
  %y1 = load i32, ptr %b1, align 1, !revenant.va !36
  %p1 = mul i32 %x1, %y1, !revenant.va !37
  %acc1.next = add i32 %acc1, %p1, !revenant.va !38
  %next = add i64 %i, 2, !revenant.va !39
  %done = icmp eq i64 %next, %n2, !revenant.va !40
  br i1 %done, label %exit, label %loop, !revenant.va !41

exit:
  %s = add i32 %acc0, %acc1, !revenant.va !42
  ret i32 %s, !revenant.va !43
}

; CHECK-LABEL: define i32 @dot_acc(
; CHECK: %ua_merge = phi i32 [ 0, %entry ], [ %ua_upd1, %loop ]
; CHECK: %next = add i64 %i, 2
; CHECK-NOT: %acc0.next
; CHECK-NOT: %acc1.next

; --- Case 4: single accumulator (should NOT fold) ---

define i32 @single_acc(ptr %a, i32 %n) {
entry:
  %n2 = zext i32 %n to i64
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %acc = phi i32 [ 0, %entry ], [ %acc.next, %loop ]
  %addr = getelementptr i32, ptr %a, i64 %i, !revenant.va !44
  %x = load i32, ptr %addr, align 1, !revenant.va !45
  %acc.next = add i32 %acc, %x, !revenant.va !46
  %next = add i64 %i, 2, !revenant.va !47
  %done = icmp eq i64 %next, %n2, !revenant.va !48
  br i1 %done, label %exit, label %loop, !revenant.va !49

exit:
  ret i32 %acc, !revenant.va !50
}

; CHECK-LABEL: define i32 @single_acc(
; CHECK-NOT: ua_merge
; CHECK-NOT: ua_upd

; --- Case 5: different initial values (should NOT fold) ---

define i32 @diff_init(ptr %a, i32 %n) {
entry:
  %n2 = zext i32 %n to i64
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %acc0 = phi i32 [ 0, %entry ], [ %acc0.next, %loop ]
  %acc1 = phi i32 [ 1, %entry ], [ %acc1.next, %loop ]
  %addr0 = getelementptr i32, ptr %a, i64 %i, !revenant.va !51
  %x = load i32, ptr %addr0, align 1, !revenant.va !52
  %acc0.next = add i32 %acc0, %x, !revenant.va !53
  %addr1 = getelementptr i32, ptr %addr0, i64 1, !revenant.va !54
  %y = load i32, ptr %addr1, align 1, !revenant.va !55
  %acc1.next = add i32 %acc1, %y, !revenant.va !56
  %next = add i64 %i, 2, !revenant.va !57
  %done = icmp eq i64 %next, %n2, !revenant.va !58
  br i1 %done, label %exit, label %loop, !revenant.va !59

exit:
  %s = add i32 %acc0, %acc1, !revenant.va !60
  ret i32 %s, !revenant.va !61
}

; CHECK-LABEL: define i32 @diff_init(
; CHECK-NOT: ua_merge
; CHECK-NOT: ua_upd

; --- Case 6: float accumulator (should NOT fold without fast-math evidence) ---

define float @float_acc(ptr %a, i32 %n) {
entry:
  %n2 = zext i32 %n to i64
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %acc0 = phi float [ 0.0, %entry ], [ %acc0.next, %loop ]
  %acc1 = phi float [ 0.0, %entry ], [ %acc1.next, %loop ]
  %addr0 = getelementptr float, ptr %a, i64 %i, !revenant.va !62
  %x = load float, ptr %addr0, align 1, !revenant.va !63
  %acc0.next = fadd float %acc0, %x, !revenant.va !64
  %addr1 = getelementptr float, ptr %addr0, i64 1, !revenant.va !65
  %y = load float, ptr %addr1, align 1, !revenant.va !66
  %acc1.next = fadd float %acc1, %y, !revenant.va !67
  %next = add i64 %i, 2, !revenant.va !68
  %done = icmp eq i64 %next, %n2, !revenant.va !69
  br i1 %done, label %exit, label %loop, !revenant.va !70

exit:
  %s = fadd float %acc0, %acc1, !revenant.va !71
  ret float %s, !revenant.va !72
}

; CHECK-LABEL: define float @float_acc(
; CHECK-NOT: ua_merge
; CHECK-NOT: ua_upd

!0 = !{i64 0}
!1 = !{i64 1}
!2 = !{i64 2}
!3 = !{i64 3}
!4 = !{i64 4}
!5 = !{i64 5}
!6 = !{i64 6}
!7 = !{i64 7}
!8 = !{i64 8}
!9 = !{i64 9}
!10 = !{i64 10}
!11 = !{i64 11}
!12 = !{i64 12}
!13 = !{i64 13}
!14 = !{i64 14}
!15 = !{i64 15}
!16 = !{i64 16}
!17 = !{i64 17}
!18 = !{i64 18}
!19 = !{i64 19}
!20 = !{i64 20}
!21 = !{i64 21}
!22 = !{i64 22}
!23 = !{i64 23}
!24 = !{i64 24}
!25 = !{i64 25}
!26 = !{i64 26}
!27 = !{i64 27}
!28 = !{i64 28}
!29 = !{i64 29}
!30 = !{i64 30}
!31 = !{i64 31}
!32 = !{i64 32}
!33 = !{i64 33}
!34 = !{i64 34}
!35 = !{i64 35}
!36 = !{i64 36}
!37 = !{i64 37}
!38 = !{i64 38}
!39 = !{i64 39}
!40 = !{i64 40}
!41 = !{i64 41}
!42 = !{i64 42}
!43 = !{i64 43}
!44 = !{i64 44}
!45 = !{i64 45}
!46 = !{i64 46}
!47 = !{i64 47}
!48 = !{i64 48}
!49 = !{i64 49}
!50 = !{i64 50}
!51 = !{i64 51}
!52 = !{i64 52}
!53 = !{i64 53}
!54 = !{i64 54}
!55 = !{i64 55}
!56 = !{i64 56}
!57 = !{i64 57}
!58 = !{i64 58}
!59 = !{i64 59}
!60 = !{i64 60}
!61 = !{i64 61}
!62 = !{i64 62}
!63 = !{i64 63}
!64 = !{i64 64}
!65 = !{i64 65}
!66 = !{i64 66}
!67 = !{i64 67}
!68 = !{i64 68}
!69 = !{i64 69}
!70 = !{i64 70}
!71 = !{i64 71}
!72 = !{i64 72}
