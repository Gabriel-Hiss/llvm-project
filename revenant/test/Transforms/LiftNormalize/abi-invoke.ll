; RUN: %revenant-opt -passes='revenant-lift-abi,verify' -S %s | %FileCheck %s
; An inttoptr callee that resolves to a function whose type differs from the
; site is rebuilt in place. The rebuild must stay an invoke: retargeting it
; with a call erases the unwind edge and leaves the block unterminated, which
; the pointer-return scan then reads as a null getTerminator(). Reduced from
; imgui-cl-o2 sub_14001E040 (invoke at bb_14001e1e2 -> unwind bb_x19462).
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

declare i32 @__CxxFrameHandler3(...)

; The call site wants i64(i64, i64) but the resolved target is void(i64), so
; retargetCall rebuilds the site; the extra arg is dropped and the result
; becomes undef.
; CHECK-LABEL: define ptr @caller(
; CHECK: invoke void @target(i64 %p)
; CHECK-NEXT: to label %ok unwind label %pad
define i64 @caller(i64 %p) #0 personality ptr @__CxxFrameHandler3 {
entry:
  %r = invoke i64 inttoptr (i64 5368971280 to ptr)(i64 %p, i64 0)
          to label %ok unwind label %pad, !revenant.va !0

ok:
  ret i64 %r

pad:
  %cl = cleanuppad within none []
  cleanupret from %cl unwind to caller
}

; Marks the caller's i64 return as a pointer: pointer-return propagation then
; walks every block terminator of @caller, which is where the old code hit
; the null terminator it had just created.
define ptr @use() #1 {
entry:
  %r = call i64 @caller(i64 7)
  %p = inttoptr i64 %r to ptr
  ret ptr %p
}

define void @target(i64 %x) #2 {
  ret void
}

!0 = !{i64 4198848}

attributes #0 = { "revenant.va"="0x140040000" }
attributes #1 = { "revenant.va"="0x140041000" }
attributes #2 = { "revenant.va"="0x140040010" }
