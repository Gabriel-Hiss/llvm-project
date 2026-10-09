; A strlen call is the c::strlen anchor: the pass rewrites it into a
; __pat_strlen call with the same argument.
; RUN: %revenant-opt -passes=revenant-patterns -S %s | %FileCheck %s

declare i64 @strlen(ptr)

define i32 @call_strlen(ptr %s) {
entry:
  ; CHECK: call i64 @__pat_strlen(ptr %s)
  ; CHECK-NOT: call i64 @strlen
  %n = call i64 @strlen(ptr %s)
  %t = trunc i64 %n to i32
  ret i32 %t
}
