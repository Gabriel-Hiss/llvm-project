; RUN: %revenant-opt -passes=revenant-unipo,verify -S %s | %FileCheck %s
; Opaque observers block the fold: the globals reach a declaration callee
; whose linked definition may record the pointer, so byte-identical
; contents do not make the objects interchangeable. The dead trap tails
; alone are not enough. Both functions and both call sites must survive.
; CHECK: define internal void @read_a(i32 %flag)
; CHECK: call void @observe_external(ptr @g_a)
; CHECK: define internal void @read_b(i32 %flag)
; CHECK: call void @observe_external(ptr @g_b)
; CHECK: define i32 @main()
; CHECK: call void @read_a(i32 0)
; CHECK: call void @read_b(i32 0)

@g_a = internal constant i32 7, align 4
@g_b = internal constant i32 7, align 4
@seen_count = external global i32
@seen_a = external global i64
@seen_b = external global i64

declare void @observe_external(ptr)
declare void @sub_0x1000()
declare void @sub_0x2000()

define internal void @read_a(i32 %flag) {
entry:
  call void @observe_external(ptr @g_a)
  %trap = icmp ne i32 %flag, 0
  br i1 %trap, label %trap.block, label %exit
trap.block:
  call void @sub_0x1000()
  unreachable
exit:
  ret void
}

define internal void @read_b(i32 %flag) {
entry:
  call void @observe_external(ptr @g_b)
  %trap = icmp ne i32 %flag, 0
  br i1 %trap, label %trap.block, label %exit
trap.block:
  call void @sub_0x2000()
  unreachable
exit:
  ret void
}

define i32 @main() {
entry:
  store i32 0, ptr @seen_count, align 4
  store i64 0, ptr @seen_a, align 8
  store i64 0, ptr @seen_b, align 8
  call void @read_a(i32 0)
  call void @read_b(i32 0)
  %a = load i64, ptr @seen_a, align 8
  %b = load i64, ptr @seen_b, align 8
  %same = icmp eq i64 %a, %b
  %result = zext i1 %same to i32
  ret i32 %result
}
