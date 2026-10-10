; RUN: %revenant-opt -passes=revenant-idioms -S %s -o - | %FileCheck %s
; Zero-argument intrinsics reach phase 3 in lifted code (cpp-gcc-o2
; sub_14001A310 lifts rdrand as `call {i32,i32} @llvm.x86.rdrand.32()`).
; The intrinsic rewrites only read operands the call carries; anything
; else passes through instead of tripping the getArgOperand bound.
declare { i32, i32 } @llvm.x86.rdrand.32() #0

define i32 @rdrand_ok(i32 %x) {
entry:
  %r = call { i32, i32 } @llvm.x86.rdrand.32()
  %v = extractvalue { i32, i32 } %r, 0
  %q = add i32 %x, %v
  ret i32 %q
}
; CHECK-LABEL: define i32 @rdrand_ok(
; CHECK: call { i32, i32 } @llvm.x86.rdrand.32()
; CHECK: ret i32 %q

attributes #0 = { nounwind }
