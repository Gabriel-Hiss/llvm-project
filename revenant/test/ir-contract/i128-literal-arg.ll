; i128 constants passed to wide-record helpers: the literal must emit as a
; compound literal of the wide record type, and a shift amount as
; `unsigned`, not a u64 that fails C2440 when the helper takes struct
; revenant_i128_u. Exercises buildIntegerLiteral on a record QualType
; plus wrapper argument conformance (revenant frontend).
source_filename = "i128-arg.exe"
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

define i64 @sub_140050B44(i64 %x) #0 {
bb_140050b44:
  %w = sext i64 %x to i128, !revenant.va !4
  %m = mul i128 %w, 2361183241434822607, !revenant.va !5
  %s = add i128 %m, 0, !revenant.va !6
  %h = shl i128 %s, 71, !revenant.va !7
  %t = trunc i128 %h to i64, !revenant.va !8
  ret i64 %t, !revenant.va !8
}

attributes #0 = { "revenant.va"="0x140050b44" }

!llvm.module.flags = !{!0}

!0 = !{i32 4, !"revenant.lang", !"c"}
!4 = !{i64 u0x140050b48}
!5 = !{i64 u0x140050b4f}
!6 = !{i64 u0x140050b57}
!7 = !{i64 u0x140050b5e}
!8 = !{i64 u0x140050b62}
