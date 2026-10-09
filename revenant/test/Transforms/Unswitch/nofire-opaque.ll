; RUN: opt -load-pass-plugin=%shlibdir/RevenantTransformUnswitch.dll -passes=revenant-unswitch -S %s | FileCheck %s
; Negative: an opaque call inside the lookup region vetoes the rewrite.
; CHECK-LABEL: define i32 @opaquekept(
; CHECK: icmp ult i32 %0, 20
; CHECK: call i32 @revenant.asm.nop(i32 %x)
declare i32 @revenant.asm.nop(i32) #0

@switch.table.opaquekept = private unnamed_addr constant [20 x i32] [i32 100, i32 100, i32 100, i32 100, i32 100, i32 200, i32 200, i32 200, i32 200, i32 200, i32 300, i32 300, i32 300, i32 300, i32 300, i32 400, i32 400, i32 400, i32 400, i32 400], align 4

define i32 @opaquekept(i32 %0, i32 %x) {
  %2 = icmp ult i32 %0, 20, !revenant.va !{i64 4096}
  br i1 %2, label %3, label %7

3:
  %4 = zext i32 %0 to i64
  %5 = getelementptr i32, ptr @switch.table.opaquekept, i64 %4
  %6 = load i32, ptr %5, align 4
  %op = call i32 @revenant.asm.nop(i32 %x), !revenant.opaque !{i64 4099}
  %m = add i32 %6, %op
  br label %7

7:
  %8 = phi i32 [ %m, %3 ], [ -1, %1 ], !revenant.va !{i64 4097}
  ret i32 %8
}

attributes #0 = { nounwind }
