; Big-endian guard: lane splitting assumes low bytes at low offsets, so a
; big-endian module must pass through untouched.
; RUN: %revenant-opt -passes=revenant-devectorize -S %s | %FileCheck %s

target datalayout = "E-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

define i32 @slp_add_be(ptr %p0) {
entry:
  %v = load <4 x i32>, ptr %p0, align 16
  %s = add <4 x i32> %v, %v
  %l = extractelement <4 x i32> %s, i32 0
  ret i32 %l
}

; CHECK: load <4 x i32>
; CHECK: add <4 x i32>
; CHECK: ret i32
