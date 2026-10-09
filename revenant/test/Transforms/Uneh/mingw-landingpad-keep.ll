; MinGW LSDA shape survival: landingpad/catch/typeid/resume under
; __gxx_personality_seh0 passes through unchanged. Corpus: cpp-gcc-o0 map
; (__gxx_personality_seh0 + .gcc_except_table); IR shape from LLVM's own
; win-mixed-ehpersonality test (landingpad/cleanup/catch/typeid/resume).
; RUN: %revenant-opt -passes=revenant-uneh -S %s | %FileCheck %s

declare i32 @__gxx_personality_seh0(...)
declare i32 @llvm.eh.typeid.for(ptr) readnone nounwind
declare void @may_throw()
@_ZTIi = external constant ptr

define i32 @use_gcc() personality ptr @__gxx_personality_seh0 {
entry:
  ; CHECK-LABEL: define i32 @use_gcc(
  ; CHECK: landingpad { ptr, i32 }
  ; CHECK: catch ptr @_ZTIi
  ; CHECK: resume { ptr, i32 }
  invoke void @may_throw() to label %cont unwind label %lpad, !revenant.va !0

cont:
  ret i32 0, !revenant.va !1

lpad:
  %eh = landingpad { ptr, i32 } cleanup catch ptr @_ZTIi, !revenant.va !0
  %sel = extractvalue { ptr, i32 } %eh, 1, !revenant.va !0
  %id = call i32 @llvm.eh.typeid.for(ptr @_ZTIi), !revenant.va !0
  %m = icmp eq i32 %sel, %id, !revenant.va !0
  br i1 %m, label %ret1, label %res, !revenant.va !0

ret1:
  ret i32 1, !revenant.va !1

res:
  resume { ptr, i32 } %eh, !revenant.va !0
}

!0 = !{i64 4198400}
!1 = !{i64 4198410}
