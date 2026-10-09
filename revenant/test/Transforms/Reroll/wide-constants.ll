; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-reroll -S %s | %FileCheck %s
; Lifted SIMD masks must not be narrowed to i64 during affine matching.
; Equal wide constants remain comparable without computing a difference.

define i128 @wide_masks(i128 %x, i128 %y, i128 %z, i128 %w) {
entry:
  %a = and i128 %x, 18446744073709551616
  %b = and i128 %y, 36893488147419103232
  %c = and i128 %z, 55340232221128654848
  %d = and i128 %w, 73786976294838206464
  %ab = or i128 %a, %b
  %cd = or i128 %c, %d
  %r = or i128 %ab, %cd
  ret i128 %r
}

; CHECK-LABEL: define i128 @wide_masks
; CHECK: and i128 %x, 18446744073709551616
; CHECK: ret i128

define i128 @wide_stride(ptr %p) {
entry:
  br label %loop
loop:
  %i = phi i128 [ 0, %entry ], [ %next, %loop ]
  %g = getelementptr i8, ptr %p, i128 %i
  %v = load i8, ptr %g
  %next = add i128 %i, 18446744073709551616
  %done = icmp eq i8 %v, 0
  br i1 %done, label %exit, label %loop
exit:
  ret i128 %next
}

; CHECK-LABEL: define i128 @wide_stride
; CHECK: add i128 %i, 18446744073709551616

define i64 @minimum_stride(ptr %p) {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %g = getelementptr i8, ptr %p, i64 %i
  %v = load i8, ptr %g
  %next = sub i64 %i, -9223372036854775808
  %done = icmp eq i8 %v, 0
  br i1 %done, label %exit, label %loop
exit:
  ret i64 %next
}

; CHECK-LABEL: define i64 @minimum_stride
; CHECK: sub i64 %i, -9223372036854775808

define i64 @overflow_difference(i64 %x, i64 %y, i64 %z, i64 %w) {
entry:
  %a = add i64 %x, -9223372036854775808
  %b = add i64 %y, 9223372036854775807
  %c = add i64 %z, -9223372036854775808
  %d = add i64 %w, 9223372036854775807
  %ab = xor i64 %a, %b
  %cd = xor i64 %c, %d
  %r = xor i64 %ab, %cd
  ret i64 %r
}

; CHECK-LABEL: define i64 @overflow_difference
; CHECK: ret i64
