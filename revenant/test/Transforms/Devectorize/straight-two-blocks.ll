; Two straight-line vector blocks get function-wide unique handling:
; each block explodes independently, no names collide, SSA stays closed.
; Ports straight_blocks_get_unique_names.
; RUN: %revenant-opt -passes=revenant-devectorize -S %s | %FileCheck %s

define i32 @slp_two(ptr %p, ptr %q) {
entry:
  %x0 = load <4 x i32>, ptr %p, align 16
  %s0 = add <4 x i32> %x0, %x0
  %e0 = extractelement <4 x i32> %s0, i32 0
  br label %B

B:
  %x1 = load <4 x i32>, ptr %q, align 16
  %s1 = add <4 x i32> %x1, %x1
  %e1 = extractelement <4 x i32> %s1, i32 0
  %t = add i32 %e0, %e1
  ret i32 %t
}

; CHECK-NOT: <4 x i32>
; CHECK: add i32
; CHECK: ret i32 %t
