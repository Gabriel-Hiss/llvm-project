; Clang 2-accumulator shuffle fold with scalar tail: my emit (scalar chains,
; s.unopt.m combine, scalar-seeded tail) as the UnloopVec collapse input.
; The piped unloopvec RUN line lands with the collapse side (MuseUnloopVecR
; fills in UNLOOP checks); until then only the DEVEC line runs.
; RUN: %revenant-opt -passes=revenant-devectorize -S %s | %FileCheck %s --check-prefix=DEVEC
define i32 @sum_fold(ptr %a, i32 %n) {
entry:
  %guard = icmp slt i32 %n, 1
  br i1 %guard, label %ret, label %pre
pre:
  %small = icmp ult i32 %n, 8
  br i1 %small, label %tail, label %setup
setup:
  %wide = and i32 %n, 2147483640
  br label %loop
loop:
  %acc.a = phi <4 x i32> [ zeroinitializer, %setup ], [ %next.a, %loop ]
  %acc.b = phi <4 x i32> [ zeroinitializer, %setup ], [ %next.b, %loop ]
  %iv = phi i32 [ 0, %setup ], [ %iv.next, %loop ]
  %ptr.a = getelementptr i32, ptr %a, i32 %iv
  %vec.a = load <4 x i32>, ptr %ptr.a, align 4
  %next.b = add <4 x i32> %acc.b, %vec.a
  %ptr.b = getelementptr <4 x i32>, ptr %ptr.a, i64 1
  %vec.b = load <4 x i32>, ptr %ptr.b, align 4
  %next.a = add <4 x i32> %acc.a, %vec.b
  %iv.next = add i32 %iv, 8
  %done = icmp slt i32 %iv.next, %wide
  br i1 %done, label %loop, label %fold
fold:
  %m = add <4 x i32> %next.a, %next.b
  %s1 = shufflevector <4 x i32> %m, <4 x i32> poison, <4 x i32> <i32 2, i32 3, i32 2, i32 3>
  %h1 = add <4 x i32> %s1, %m
  %s2 = shufflevector <4 x i32> %h1, <4 x i32> poison, <4 x i32> <i32 1, i32 poison, i32 poison, i32 poison>
  %h2 = add <4 x i32> %s2, %h1
  %r = extractelement <4 x i32> %h2, i64 0
  br label %tail
tail:
  %acc = phi i32 [ 0, %pre ], [ %r, %fold ]
  %idx = phi i32 [ 0, %pre ], [ %wide, %fold ]
  %more = icmp slt i32 %idx, %n
  br i1 %more, label %body, label %ret
body:
  %p = getelementptr i32, ptr %a, i32 %idx
  %v = load i32, ptr %p, align 4
  %acc.n = add i32 %acc, %v
  %idx.n = add i32 %idx, 1
  %done2 = icmp slt i32 %idx.n, %n
  br i1 %done2, label %body, label %ret
ret:
  %out = phi i32 [ 0, %entry ], [ %acc, %tail ], [ %acc.n, %body ]
  ret i32 %out
}

; DEVEC: s.unopt = phi i32 [ 0, %setup ]
; DEVEC: s.unopt.m = add i32 {{.*}}, {{.*}}
; DEVEC: %acc = phi i32 [ 0, %pre ], [ %s.unopt.m, %fold ]
; DEVEC-NOT: <4 x i32>
; DEVEC: ret i32
