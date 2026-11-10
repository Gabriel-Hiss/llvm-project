; RUN: %revenant-opt -passes='function(revenant-lift-normalize),verify' -S %s | %FileCheck %s
; An unoptimized frame whose address escapes splits into objects. The lifter
; computes `rsp + k` for every access, so a field the function writes before
; handing out the object's address has an address computation of its own
; that nothing reads. That is not an escape: the field stays in the object
; the callee receives (an iterator's two words here), not in a slot of its
; own that looks write-only.
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"

declare i64 @next(i64)

; CHECK-LABEL: define i64 @iterate(
; CHECK: %stack_0 = alloca
; CHECK-NOT: %stack_8
; CHECK: store volatile i64 %begin, ptr %stack_0
; CHECK: %[[HI:.*]] = getelementptr i64, ptr %stack_0, i64 1
; CHECK: store volatile i64 %end, ptr %[[HI]]
; CHECK: ret i64
define i64 @iterate(i64 %begin, i64 %end) "revenant.o0" {
  %frame = alloca [32 x i8], align 1
  store i64 %begin, ptr %frame, align 1
  %hi = getelementptr inbounds i8, ptr %frame, i64 8
  %dead = ptrtoint ptr %hi to i64
  store i64 %end, ptr %hi, align 1
  %arg = ptrtoint ptr %frame to i64
  %r = call i64 @next(i64 %arg)
  ret i64 %r
}
