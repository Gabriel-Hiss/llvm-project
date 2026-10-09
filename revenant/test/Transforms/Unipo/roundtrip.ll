; Forward round-trip: stock deadargelim turns the pre shape (unused i32
; return, dead at every caller) into the post shape (void), and
; revenant-unipo inverts it back.
; RUN: %opt -passes=deadargelim -S %s -o - | %opt -load-pass-plugin=%plugin -passes=revenant-unipo -S - | %FileCheck %s

; CHECK: define internal i32 @roundtrip(ptr %a, i32 %n)
; CHECK: ret i32 %v
define internal i32 @roundtrip(ptr %a, i32 %n) {
entry:
  %v = call i32 @produces(ptr %a, i32 %n)
  ret i32 %v
}

define void @caller(ptr %a, i32 %n) {
entry:
  call i32 @roundtrip(ptr %a, i32 %n)
  ret void
}

declare i32 @produces(ptr, i32)
