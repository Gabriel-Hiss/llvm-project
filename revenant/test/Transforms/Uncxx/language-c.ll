; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-uncxx -S %s | %FileCheck %s
; The module language flag must stop STL recovery on C IR, even when a
; struct happens to contain two pointer fields at the vector first/last offsets.
; CHECK-LABEL: define i1 @c_pointer_fields_equal
; CHECK-NOT: revenant.cxx.vector.empty
; CHECK: icmp eq ptr %first, %last

target triple = "x86_64-pc-windows-msvc"

define i1 @c_pointer_fields_equal(ptr %record) {
entry:
  %first = load ptr, ptr %record, align 8
  %last.addr = getelementptr i8, ptr %record, i64 8
  %last = load ptr, ptr %last.addr, align 8
  %equal = icmp eq ptr %first, %last
  ret i1 %equal
}

!llvm.module.flags = !{!0}
!0 = !{i32 4, !"revenant.lang", !"c"}
