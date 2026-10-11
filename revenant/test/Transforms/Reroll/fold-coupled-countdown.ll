; RUN: %revenant-opt -passes=revenant-reroll,verify -S %s | %FileCheck %s
; Coupled-countdown rescale differential. The countdown's entry is a
; CONSTANT, the only shape exitTripCompatible accepts for it, and the fold
; multiplies that entry by the window count: the rolled loop runs D times
; as many iterations at step 1, so the trip counter must start at D times
; the value to fire at the same total element count.
;
; Source: `s = 0; for (i = 0; i < 4; i += 2) { s += a[i]*b[i];
; s += a[i+1]*b[i+1]; }`

define i32 @dot4_const(ptr %a, ptr %b) {
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
  %r11n = add i32 %r11, -1
  %more = icmp eq i32 %r11n, 0
  br i1 %more, label %out, label %loop

out:
  %sum = add i32 %r8n, %rdxn
  ret i32 %sum
}

; CHECK-LABEL: define i32 @dot4_const(
; The countdown entry is rescaled 2 -> 4: two iterations at step 2 cover
; four elements, and so do four iterations at step 1.
; CHECK: %r11 = phi i32 [ 4, %entry ], [ %r11n, %loop ]
; The element step folds 2 -> 1.
; CHECK: %d = add i64 %rax, 1
; One window survives and the two accumulators collapse into one.
; CHECK: %rdxn = add i32 %rdx, %p0
; CHECK-NOT: %p1 = mul i32
; CHECK-NOT: %r8 = phi i32
; CHECK: %r11n = add i32 %r11, -1
; CHECK: %more = icmp eq i32 %r11n, 0
; The exit merge collapses to the single rolled accumulator.
; CHECK: ret i32 %rdxn
