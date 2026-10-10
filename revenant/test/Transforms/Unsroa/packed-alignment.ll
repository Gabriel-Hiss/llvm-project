; RUN: %revenant-opt -passes=revenant-unsroa -S %s | %FileCheck %s
;
; Packed scalar slots keep at most commonAlignment(base align, offset):
; an i8 member at offset 0 keeps align 1 while the i32 at offset 1 drops
; from align 4 to align 1 (the packed address is only 1-aligned).
; Before the fix both accesses kept align 4 (misaligned-access UB).

target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"

define i32 @read_word() {
; CHECK-LABEL: define i32 @read_word(
entry:
  %v.sroa.0 = alloca i8, align 1
  %v.sroa.1 = alloca i32, align 4
  store i8 7, ptr %v.sroa.0, align 1
  store i32 287454020, ptr %v.sroa.1, align 4
; CHECK: store i8 7, ptr {{.*}}, align 1
; CHECK: store i32 287454020, ptr {{.*}}, align 1
  %value = load i32, ptr %v.sroa.1, align 4
; CHECK: %value = load i32, ptr {{.*}}, align 1
  ret i32 %value
}
