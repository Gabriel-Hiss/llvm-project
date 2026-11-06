; RUN: %revenant-opt -passes=revenant-stackprotector -S %s | %FileCheck %s
; An optimized function whose /GS cookie was removed gets the stack
; protector the compiler's IR carries right before instruction selection.
; An unoptimized one, or one that never had a cookie, stays as it is.

; CHECK-LABEL: define i32 @protected(
; CHECK-NEXT: %StackGuardSlot = alloca ptr
; CHECK-NEXT: %[[G:.+]] = call ptr @llvm.stackguard()
; CHECK-NEXT: call void @llvm.stackprotector(ptr %[[G]], ptr %StackGuardSlot)
define i32 @protected(i32 %a) "rv.gscookie"="1" {
  ret i32 %a
}

; CHECK-LABEL: define i32 @unoptimized(
; CHECK-NOT: stackguard
; CHECK: ret i32
define i32 @unoptimized(i32 %a) "rv.gscookie"="1" "revenant.o0" {
  ret i32 %a
}

; CHECK-LABEL: define i32 @plain(
; CHECK-NOT: stackguard
; CHECK: ret i32
define i32 @plain(i32 %a) {
  ret i32 %a
}
