; lshr/shl i128: IR shift counts carry the operand width, so the count
; reaches the emitter as a wide record. The wrapper signature wants
; `unsigned`; the count must be narrowed to the record's low half or MSVC
; rejects the call with C2440 (zlib-cl-o2 magic-div sites, e.g. the
; 0xA57EB50395BBCDAD-divide shape below).
source_filename = "wide-i128-shift.exe"
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

define i64 @sub_140050B00(i64 %a) #0 {
bb_140050b00:
  %w = zext i64 %a to i128, !revenant.va !1
  %m = mul i128 %w, 14757395258967641293, !revenant.va !2
  %s = lshr i128 %m, 67, !revenant.va !3
  %l = trunc i128 %s to i64, !revenant.va !4
  ret i64 %l, !revenant.va !5
}

attributes #0 = { "revenant.va"="0x140050b00" }

!llvm.module.flags = !{!0}

!0 = !{i32 4, !"revenant.lang", !"c"}
!1 = !{i64 u0x140050b04}
!2 = !{i64 u0x140050b10}
!3 = !{i64 u0x140050b14}
!4 = !{i64 u0x140050b18}
!5 = !{i64 u0x140050b1c}
