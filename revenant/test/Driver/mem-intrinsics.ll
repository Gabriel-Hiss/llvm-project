; llvm.memset/memcpy/memmove the lifter leaves behind are the CRT's own
; calls: the emitter prints them as memset/memcpy/memmove and pulls in
; <string.h> for their prototypes, so the recompiled TU resolves them at
; link time instead of referencing a sanitized intrinsic name.
; RUN: %revenant-clang %s --lang c -o %t.c && %FileCheck %s --check-prefix=MEMC < %t.c

@g = global [16 x i8] zeroinitializer

define void @clear(i8 *%dst, i8 %val) {
entry:
  call void @llvm.memset.p0.i64(ptr %dst, i8 %val, i64 16, i1 false)
  ret void
}

define void @copy(i8 *%dst, i8 *%src) {
entry:
  call void @llvm.memcpy.p0.p0.i64(ptr %dst, ptr %src, i64 16, i1 false)
  ret void
}

define void @slide(i8 *%dst, i8 *%src) {
entry:
  call void @llvm.memmove.p0.p0.i64(ptr %dst, ptr %src, i64 16, i1 false)
  ret void
}

declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)
declare void @llvm.memmove.p0.p0.i64(ptr, ptr, i64, i1)

; MEMC: #include <string.h>
; MEMC: memset(
; MEMC: memcpy(
; MEMC: memmove(
; MEMC-NOT: llvm_2ememset
; MEMC-NOT: llvm_2ememcpy
; MEMC-NOT: llvm_2ememmove
