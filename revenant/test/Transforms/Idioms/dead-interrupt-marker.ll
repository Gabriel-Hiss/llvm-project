; The trap marker a compiler leaves behind a noreturn call (int3 after a
; throw, __fastfail after a panic) is not part of the source: the block
; already ends dead, so the interrupt call is dropped. An interrupt inside
; a live block stays.
; RUN: %revenant-opt -passes=revenant-idioms -S %s -o - | %FileCheck %s

declare void @llvm.x86.int(i8 immarg)
declare void @_CxxThrowException(i64, i64, i64, i64)

define void @throw_marker(i64 %obj) {
entry:
  call void @_CxxThrowException(i64 %obj, i64 0, i64 0, i64 0)
  call void @llvm.x86.int(i8 3)
  unreachable
}

define void @debug_break(i64 %n) {
entry:
  %c = icmp sgt i64 %n, 0
  br i1 %c, label %trap, label %done
trap:
  call void @llvm.x86.int(i8 3)
  br label %done
done:
  ret void
}

; CHECK-LABEL: define void @throw_marker(
; CHECK-NOT: call void @llvm.x86.int
; CHECK: unreachable

; CHECK-LABEL: define void @debug_break(
; CHECK: call void @llvm.x86.int(i8 3)
