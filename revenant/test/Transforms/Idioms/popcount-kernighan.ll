; RUN: %revenant-opt -passes=revenant-idioms -S %s -o - | %FileCheck %s
; Kernighan's source-level popcount loop remains a loop, matching clang -O0.
define i32 @popcount_loop(i32 %x) {
entry:
  br label %loop
loop:
  %remaining = phi i32 [ %x, %entry ], [ %next, %body ]
  %count = phi i32 [ 0, %entry ], [ %incremented, %body ]
  %nonzero = icmp ne i32 %remaining, 0
  br i1 %nonzero, label %body, label %exit
body:
  %minus_one = sub i32 %remaining, 1
  %next = and i32 %remaining, %minus_one
  %incremented = add i32 %count, 1
  br label %loop
exit:
  ret i32 %count
}
; CHECK-LABEL: define i32 @popcount_loop(
; CHECK: %count = phi i32
; CHECK: br i1 %nonzero
; CHECK: and i32 %remaining, %minus_one
