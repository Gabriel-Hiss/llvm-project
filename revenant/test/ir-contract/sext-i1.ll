; sext i1 to i32: IR i1 has no signed C counterpart, so the emitter must not
; ask ASTContext for one (ASTContext.cpp getCorrespondingSignedType asserts on
; BoolTy). The correct spelling is a negation: -(int)x.
source_filename = "sext-i1.exe"
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

define i32 @sub_14007C970(i32 %a, i32 %b) #0 {
bb_14007c970:
  %cf = alloca i1, align 1
  %r = alloca i32, align 4
  %c = icmp ult i32 %a, %b, !revenant.va !4
  br i1 %c, label %bb_x0, label %bb_14007c9b2, !revenant.va !5

bb_x0:
  %cf433 = load i1, ptr %cf, align 1
  %.neg = sext i1 %cf433 to i32
  %not = xor i1 %cf433, true
  %w = zext i1 %not to i32
  %sum = add nsw i32 %.neg, %w, !revenant.va !6
  store i32 %sum, ptr %r, align 4
  br label %bb_14007c9b2

bb_14007c9b2:
  %out = load i32, ptr %r, align 4
  ret i32 %out, !revenant.va !7
}

attributes #0 = { "revenant.va"="0x14007c970" }

!llvm.module.flags = !{!0}

!0 = !{i32 4, !"revenant.lang", !"c"}
!4 = !{i64 u0x14007c974}
!5 = !{i64 u0x14007c978}
!6 = !{i64 u0x14007ca10}
!7 = !{i64 u0x14007c9b4}
