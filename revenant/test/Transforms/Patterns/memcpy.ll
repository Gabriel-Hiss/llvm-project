; llvm.memset / llvm.memcpy intrinsics are the c::memset / c::memcpy
; anchors. The pass rewrites each into the matching __pat_* helper with
; the routine operands (the trailing i1 isVolatile flag is dropped).
; RUN: %revenant-opt -passes=revenant-patterns -S %s | %FileCheck %s

declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)

define void @fill(ptr %d, i8 %v, i64 %n) {
entry:
  ; CHECK: call void @__pat_memset(ptr %d, i8 %v, i64 %n)
  ; CHECK-NOT: llvm.memset
  call void @llvm.memset.p0.i64(ptr %d, i8 %v, i64 %n, i1 false)
  ret void
}

define void @copy(ptr %d, ptr %s, i64 %n) {
entry:
  ; CHECK: call void @__pat_memcpy(ptr %d, ptr %s, i64 %n)
  ; CHECK-NOT: llvm.memcpy
  call void @llvm.memcpy.p0.p0.i64(ptr %d, ptr %s, i64 %n, i1 false)
  ret void
}
