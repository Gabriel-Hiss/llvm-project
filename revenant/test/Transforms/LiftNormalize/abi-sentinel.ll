; RUN: %revenant-opt -passes='revenant-lift-abi,verify' -S %s | %FileCheck %s
; inttoptr constants of -1/-2 are register sentinels, not image VAs. They are
; also DenseMap's empty/tombstone keys for uint64_t: an unsentinel-guarded
; Functions.find(-1) false-hits an empty bucket and hands back garbage, which
; a phi operand U.set then dereferences. Reduced from zlib-cl-o2
; __acrt_fp_strflt_to_string (0x14007071C) where the pass segfaulted.
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

@g_14005000 = internal constant [16 x i8] zeroinitializer, !revenant.va !0
@g_14006000 = internal constant [16 x i8] zeroinitializer, !revenant.va !1

; CHECK-LABEL: define void @walker(
; CHECK: phi ptr [ %inc, %loop ], [ inttoptr (i64 -1 to ptr), %entry ]
; CHECK: phi ptr [ %q, %loop ], [ inttoptr (i64 -2 to ptr), %entry ]
define void @walker() #0 {
entry:
  br label %loop

loop:
  %p = phi ptr [ %inc, %loop ], [ inttoptr (i64 -1 to ptr), %entry ]
  %t = phi ptr [ %q, %loop ], [ inttoptr (i64 -2 to ptr), %entry ]
  %v = load i8, ptr %p
  %inc = getelementptr i8, ptr %p, i64 1
  %q = getelementptr i8, ptr %t, i64 1
  %done = icmp eq i8 %v, 0
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; Image-backed addresses still resolve through the same operand walk.
; CHECK-LABEL: define void @absuser(
; CHECK: phi ptr [ @g_14005000, %a ], [ @g_14005000, %b ]
define void @absuser(i1 %c) #1 {
entry:
  br i1 %c, label %a, label %b

a:
  br label %m

b:
  br label %m

m:
  %x = phi ptr [ inttoptr (i64 5368713216 to ptr), %a ], [ inttoptr (i64 5368713216 to ptr), %b ]
  store i8 0, ptr %x
  ret void
}

!0 = !{i64 5368713216}
!1 = !{i64 5368717312}

attributes #0 = { "revenant.va"="0x14001000" }
attributes #1 = { "revenant.va"="0x14002000" }
