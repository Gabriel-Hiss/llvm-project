; Single-use pure defs stay inline (no local, no DI); a load feeding
; a use materializes into one named local with one store and loads.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-ssaout -S %s | %FileCheck %s

; CHECK: define i32 @prop
; CHECK-NOT: alloca
; CHECK: ret i32
; CHECK-NOT: alloca

define i32 @prop(i32 %a, i32 %b, i32 %c) {
entry:
  %x = add i32 %a, %b, !revenant.va !0
  %y = add i32 %x, %c, !revenant.va !0
  %z = mul i32 %x, %y, !revenant.va !0
  ret i32 %z, !revenant.va !0
}

; CHECK: define i32 @mat
; CHECK: alloca i32
; CHECK: store i32
; CHECK: ret i32

define i32 @mat(ptr %p, i32 %q) {
entry:
  %v = load i32, ptr %p, !revenant.va !0
  %a = add i32 %v, %q, !revenant.va !0
  %b = mul i32 %v, %a, !revenant.va !0
  ret i32 %b, !revenant.va !0
}

!0 = !{i64 4198496}
