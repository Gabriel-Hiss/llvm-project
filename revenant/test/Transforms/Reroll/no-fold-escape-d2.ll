; RUN: %revenant-opt -passes=revenant-reroll,verify -S %s | %FileCheck %s
; The D==2 escape gate. Window 1's product reaches a live value outside
; the loop that is neither the accumulator merge nor a phi, so the fold
; cannot supply a post-loop equivalent for it. Folding would leave both
; accumulators summing the same products after the remap (the doubling
; hazard), so the fold refuses.
;
; The companion fixture fold-coupled-countdown.ll is this exact shape
; without the escape and does fold, which pins the escape as the cause.

define i32 @double_escape(ptr %a, ptr %b) {
entry:
  br label %loop

loop:
  %r11 = phi i32 [ 2, %entry ], [ %r11n, %loop ]
  %r8 = phi i32 [ 0, %entry ], [ %r8n, %loop ]
  %rdx = phi i32 [ 0, %entry ], [ %rdxn, %loop ]
  %rax = phi i64 [ 0, %entry ], [ %d, %loop ]
  %ea = getelementptr i32, ptr %a, i64 %rax
  %x0 = load i32, ptr %ea, align 1
  %eb = getelementptr i32, ptr %b, i64 %rax
  %y0 = load i32, ptr %eb, align 1
  %p0 = mul i32 %x0, %y0
  %d = add i64 %rax, 2
  %rdxn = add i32 %rdx, %p0
  %sc = shl i64 %d, 2
  %ab = getelementptr i8, ptr %b, i64 %sc
  %eb2 = getelementptr i32, ptr %ab, i64 -1
  %x1 = load i32, ptr %eb2, align 1
  %aa = getelementptr i8, ptr %a, i64 %sc
  %ea2 = getelementptr i32, ptr %aa, i64 -1
  %y1 = load i32, ptr %ea2, align 1
  %p1 = mul i32 %x1, %y1
  %r8n = add i32 %r8, %p1
  ; The escape: window 1's product reaches a live value the accumulator
  ; merge does not consume.
  %leak = mul i32 %p1, 3
  %r11n = add i32 %r11, -1
  %more = icmp eq i32 %r11n, 0
  br i1 %more, label %out, label %loop

out:
  %sum = add i32 %r8n, %rdxn
  %total = add i32 %sum, %leak
  ret i32 %total
}

; CHECK-LABEL: define i32 @double_escape(
; No fold: every load stays and both accumulators stay distinct. CHECKs are
; in block order because FileCheck scans forward.
; The second accumulator's phi survives.
; CHECK: %r8 = phi i32 [ 0, %entry ], [ %r8n, %loop ]
; CHECK: %p0 = mul i32 %x0, %y0
; The step is not divided.
; CHECK: %d = add i64 %rax, 2
; CHECK: %rdxn = add i32 %rdx, %p0
; CHECK: %p1 = mul i32 %x1, %y1
; CHECK: %r8n = add i32 %r8, %p1
; CHECK: %leak = mul i32 %p1, 3
; CHECK-NOT: add i64 %rax, 1$
