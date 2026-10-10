; RUN: %revenant-opt -passes=revenant-unipo,verify -S %s | %FileCheck %s
; A volatile load through the pointer makes the accessed object
; observable. Two different globals are always different, even with
; identical contents, so the volatile read of g_b must never become a
; volatile read of g_a. Both functions survive.
; CHECK: define internal i32 @read_a()
; CHECK: call i32 @observe_volatile(ptr @g_a)
; CHECK: define internal i32 @read_b()
; CHECK: call i32 @observe_volatile(ptr @g_b)
; CHECK: define i32 @main()
; CHECK: call i32 @read_b()

@g_a = internal constant i32 7, section ".mmio.a", align 4
@g_b = internal constant i32 7, section ".mmio.b", align 4

define internal i32 @observe_volatile(ptr %p) {
entry:
  %value = load volatile i32, ptr %p, align 4
  ret i32 %value
}

define internal i32 @read_a() {
entry:
  %value = call i32 @observe_volatile(ptr @g_a)
  ret i32 %value
}

define internal i32 @read_b() {
entry:
  %value = call i32 @observe_volatile(ptr @g_b)
  ret i32 %value
}

define i32 @main() {
entry:
  %value = call i32 @read_b()
  ret i32 %value
}
