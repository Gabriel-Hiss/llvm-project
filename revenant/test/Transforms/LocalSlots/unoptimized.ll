; RUN: %revenant-opt -passes='revenant-o0-module,function(revenant-locals-to-slots<unoptimized>)' -S %s | %FileCheck %s
;
; The ir-final storage of a C/C++ binary. In a binary most of whose user
; functions spilled their arguments (`revenant.o0` from cleanup), every
; function no library signature identified is unoptimized and gets -O0
; slots, even one without arguments to spill. Identified library code keeps
; its SSA values, as does every function of an optimized binary.

; CHECK-LABEL: define i32 @spilled(
; CHECK: alloca i32
define i32 @spilled(i32 %a) #0 {
  ret i32 %a
}

; CHECK-LABEL: define i32 @spilled2(
; CHECK: alloca i32
define i32 @spilled2(i32 %a) #0 {
  ret i32 %a
}

; Optimized-looking, but the binary is -O0 and the function its own.
; CHECK-LABEL: define i32 @plain(
; CHECK: alloca i32
define i32 @plain(i32 %a) #1 {
  ret i32 %a
}

; CHECK-LABEL: define i32 @noargs(i1 %c) #{{[0-9]+}} {
; CHECK: %retval = alloca i32
define i32 @noargs(i1 %c) {
entry:
  br i1 %c, label %a, label %b
a:
  br label %join
b:
  br label %join
join:
  %r = phi i32 [ 1, %a ], [ 2, %b ]
  ret i32 %r
}

; CHECK-LABEL: define i32 @crt(
; CHECK-NOT: alloca
; CHECK: ret i32 %a
define i32 @crt(i32 %a) #2 {
  ret i32 %a
}

attributes #0 = { "revenant.o0" "revenant.user"="true" }
attributes #1 = { "revenant.user"="true" }
attributes #2 = { "revenant.lib"="memcpy" }
