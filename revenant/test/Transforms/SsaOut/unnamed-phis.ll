; Two unnamed accumulator phis of one type, reduced from c-clang-o2
; sum_vec (0x140001680). Each needs its own local: a shared one makes the
; second latch store overwrite the first accumulator.
; RUN: %revenant-opt -passes=revenant-ssaout -S %s | %FileCheck %s

; CHECK-LABEL: define <4 x i32> @sum2(
; CHECK-DAG: %[[A:v_tmp]] = alloca <4 x i32>
; CHECK-DAG: %[[B:v_tmp[0-9]+]] = alloca <4 x i32>
; CHECK: load <4 x i32>, ptr %[[A]]
; CHECK: add <4 x i32>
; CHECK: load <4 x i32>, ptr %[[B]]
; CHECK: mul <4 x i32>
; CHECK: loop.loop_crit_edge:
; CHECK-NEXT: store i64
; CHECK-NEXT: store <4 x i32> %a, ptr %[[A]]
; CHECK-NEXT: store <4 x i32> %b, ptr %[[B]]
; CHECK-NEXT: br label %loop

define <4 x i32> @sum2(ptr %p, i64 %n) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %0 = phi <4 x i32> [ zeroinitializer, %entry ], [ %a, %loop ]
  %1 = phi <4 x i32> [ zeroinitializer, %entry ], [ %b, %loop ]
  %ptr = getelementptr <4 x i32>, ptr %p, i64 %i
  %v = load <4 x i32>, ptr %ptr
  %a = add <4 x i32> %0, %v
  %b = mul <4 x i32> %1, %v
  %i.next = add i64 %i, 1
  %c = icmp ult i64 %i.next, %n
  br i1 %c, label %loop, label %exit

exit:
  %r = sub <4 x i32> %a, %b
  ret <4 x i32> %r
}

; An unnamed phi fed by a named one that is still live afterwards (x is
; read in done) keeps its own local.
; CHECK-LABEL: define i64 @mixed(
; CHECK-DAG: %v_x = alloca i64
; CHECK-DAG: %v_tmp = alloca i64
; CHECK: ret i64

define i64 @mixed(i64 %n) {
entry:
  br label %head

head:
  %x = phi i64 [ 0, %entry ], [ %x1, %head ]
  %x1 = add i64 %x, 3
  %c = icmp ult i64 %x1, %n
  br i1 %c, label %head, label %tail

tail:
  %0 = phi i64 [ %x, %head ], [ %d, %tail ]
  %d = add i64 %0, -1
  %z = icmp eq i64 %d, 0
  br i1 %z, label %done, label %tail

done:
  %r = add i64 %d, %x
  ret i64 %r
}

; An unnamed phi fed by a named one that dies there joins the named web.
; CHECK-LABEL: define i64 @join(
; CHECK: %v_x = alloca i64
; CHECK-NOT: alloca
; CHECK: ret i64

define i64 @join(i64 %n) {
entry:
  br label %head

head:
  %x = phi i64 [ 0, %entry ], [ %x1, %head ]
  %x1 = add i64 %x, 3
  %c = icmp ult i64 %x1, %n
  br i1 %c, label %head, label %tail

tail:
  %0 = phi i64 [ %x, %head ], [ %d, %tail ]
  %d = add i64 %0, -1
  %z = icmp eq i64 %d, 0
  br i1 %z, label %done, label %tail

done:
  ret i64 %d
}

; Two unnamed loops one after the other are never live together: they
; share one temporary.
; CHECK-LABEL: define i64 @sequential(
; CHECK: %v_tmp = alloca i64
; CHECK-NOT: alloca i64
; CHECK: ret i64

define i64 @sequential(i64 %n) {
entry:
  br label %a

a:
  %0 = phi i64 [ 0, %entry ], [ %a1, %a ]
  %a1 = add i64 %0, 1
  %ac = icmp ult i64 %a1, %n
  br i1 %ac, label %a, label %mid

mid:
  br label %b

b:
  %1 = phi i64 [ %a1, %mid ], [ %b1, %b ]
  %b1 = add i64 %1, 2
  %bc = icmp ult i64 %b1, 100
  br i1 %bc, label %b, label %done

done:
  ret i64 %b1
}
