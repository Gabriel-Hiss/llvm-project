; RUN: %revenant-opt -passes='function(revenant-lift-normalize),verify' -S %s | %FileCheck %s
; A phi mixing pointer-derived and plain constant inedges stays integer. The
; -1 is a register sentinel, not an image address, so the inttoptr fallback
; must not turn it into a ConstantExpr incoming on a ptr phi. Reduced from
; zlib-cl-o2 __acrt_fp_strflt_to_string.
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "x86_64-pc-windows-msvc"

@g = external global i8

; CHECK-LABEL: define i64 @loop(
; CHECK: %r = phi i64 [ %n, %back ], [ -1, %entry ]
; CHECK-NEXT: %n = add i64 %r, 1
; CHECK-NEXT: %p = inttoptr i64 %r to ptr
; CHECK-NOT: phi ptr
define i64 @loop(i64 %arg) {
entry:
  br label %hdr

hdr:
  %r = phi i64 [ %n, %back ], [ -1, %entry ]
  %n = add i64 %r, 1
  %p = inttoptr i64 %r to ptr
  %b = load i8, ptr %p
  %done = icmp eq i8 %b, 0
  br i1 %done, label %out, label %back

back:
  br label %hdr

out:
  ret i64 %r
}

; A ptrtoint-of-symbol inedge is pointer-derivable, so the pointer phi is
; still built and unwraps to the global directly.
; CHECK-LABEL: define ptr @good(
; CHECK: %r.ptr = phi ptr [ %address, %back ], [ @g, %entry ]
define ptr @good(i64 %arg) {
entry:
  br label %hdr

hdr:
  %r = phi i64 [ %n, %back ], [ ptrtoint (ptr @g to i64), %entry ]
  %n = add i64 %r, 1
  %p = inttoptr i64 %r to ptr
  %b = load i8, ptr %p
  %done = icmp eq i8 %b, 0
  br i1 %done, label %out, label %back

back:
  br label %hdr

out:
  ret ptr %p
}
