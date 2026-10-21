; A counted self-loop whose latch bound is a plain constant (icmp eq i64
; %iv.next, 128). The vector-tier bound matcher called
; cast<Instruction> on the bound; a ConstantInt bound asserted
; "cast<Ty>() argument of incompatible type!" (Casting.h:572). Reduced
; from sodium-clang-o2 @sub_1400265C0, which aborted inside
; revenant-unloopvec on the corpus sweep.
; RUN: %revenant-opt -passes=revenant-unloopvec -S %s | %FileCheck %s
; CHECK-LABEL: @sub_1400265C0
; CHECK: ret i64

declare void @llvm.x86.int(i8 immarg)
declare i64 @sub_14001C970(ptr, i64)
declare i64 @sub_140026840(ptr, i8)
declare i32 @sub_14002A290(ptr, i64, i64, i64)
declare i64 @sub_14003F9F0(i64)
declare i64 @sub_1400AD000(ptr, ptr, i64)

define i64 @sub_1400265C0(ptr %0, ptr %1) {
bb_1400265c0:
  %frame = alloca [2456 x i8], align 1
  %sp0 = getelementptr inbounds nuw i8, ptr %frame, i64 2160
  %fp31 = getelementptr inbounds nuw i8, ptr %frame, i64 8
  %rsp32 = ptrtoint ptr %fp31 to i64
  %2 = xor i64 %rsp32, 47936899621426
  %fp41 = getelementptr inbounds nuw i8, ptr %frame, i64 2088
  store i64 %2, ptr %fp41, align 1
  %c = icmp eq ptr %0, null
  %c58 = icmp eq ptr %1, null
  %3 = or i1 %c58, %c
  br i1 %3, label %bb_1400267f5, label %bb_1400265fc

bb_1400265fc:
  %4 = load ptr, ptr %1, align 1
  %element4 = getelementptr ptr, ptr %4, i64 1
  %5 = load ptr, ptr %element4, align 1
  %field.bits = ptrtoint ptr %5 to i64
  %element = getelementptr i32, ptr %1, i64 8
  %6 = load i32, ptr %element, align 1
  %z81 = zext i32 %6 to i64
  %7 = shl nuw nsw i64 %z81, 10
  %a = add i64 %7, %field.bits
  %8 = add i64 %a, -1024
  %fp104 = getelementptr inbounds nuw i8, ptr %frame, i64 1064
  %9 = ptrtoint ptr %fp104 to i64
  %address = getelementptr i8, ptr %5, i64 %7
  %address3 = getelementptr i8, ptr %address, i64 -1024
  %10 = call i64 @sub_1400AD000(ptr nonnull %fp104, ptr %address3, i64 1024)
  %element5 = getelementptr i32, ptr %1, i64 9
  %11 = load i32, ptr %element5, align 1
  %z112 = zext i32 %11 to i64
  %c126 = icmp ult i32 %11, 2
  br i1 %c126, label %bb_14002679e, label %bb_14002663f

bb_14002663f:
  %12 = ptrtoint ptr %fp41 to i64
  %sc129 = shl i32 %6, 1
  %narrow657 = add i32 %sc129, -1
  %d134 = add i64 %field.bits, 1024
  br label %bb_140026682

bb_140026670:
  %13 = add i64 %r11.0, 1
  %14 = add nuw nsw i64 %16, 1
  %15 = add i32 %rbp.0.i32, %6
  %c214 = icmp eq i64 %13, %z112
  br i1 %c214, label %bb_14002679e, label %bb_140026682

bb_140026682:
  %r12.0.i32 = phi i64 [ 0, %bb_14002663f ], [ %14, %bb_140026670 ]
  %r11.0 = phi i64 [ 1, %bb_14002663f ], [ %13, %bb_140026670 ]
  %rbp.0.i32 = phi i32 [ %narrow657, %bb_14002663f ], [ %15, %bb_140026670 ]
  %16 = and i64 %r12.0.i32, 4294967295
  %z217 = zext i32 %rbp.0.i32 to i64
  %17 = shl nuw nsw i64 %z217, 10
  %t228 = trunc i64 %r12.0.i32 to i32
  %narrow660 = mul i32 %6, %t228
  %18 = add i32 %narrow660, %narrow657
  %z249 = zext i32 %18 to i64
  %19 = shl nuw nsw i64 %z249, 10
  %a259 = add i64 %d134, %19
  %c275.not = icmp ugt i64 %a259, %9
  br i1 %c275.not, label %bb_1400266a0, label %bb_140026710

bb_1400266a0:
  %20 = add i64 %19, %field.bits
  %c306.not = icmp ult i64 %20, %12
  br i1 %c306.not, label %bb_1400266a8, label %bb_140026710

bb_1400266a8:
  br label %bb_1400266c0

bb_1400266c0:
  %r9.0 = phi i64 [ 0, %bb_1400266a8 ], [ %34, %bb_1400266c0 ]
  %sc337 = shl i64 %r9.0, 3
  %address871 = getelementptr i8, ptr %5, i64 %17
  %address666 = getelementptr i8, ptr %address871, i64 %sc337
  %21 = load i64, ptr %address666, align 1
  %22 = getelementptr i8, ptr %sp0, i64 %sc337
  %element6 = getelementptr i64, ptr %22, i64 -137
  %23 = load i64, ptr %element6, align 1
  %24 = xor i64 %23, %21
  store i64 %24, ptr %element6, align 1
  %element7 = getelementptr i64, ptr %address666, i64 1
  %25 = load i64, ptr %element7, align 1
  %element8 = getelementptr i64, ptr %22, i64 -136
  %26 = load i64, ptr %element8, align 1
  %27 = xor i64 %26, %25
  store i64 %27, ptr %element8, align 1
  %element9 = getelementptr i64, ptr %address666, i64 2
  %28 = load i64, ptr %element9, align 1
  %element10 = getelementptr i64, ptr %22, i64 -135
  %29 = load i64, ptr %element10, align 1
  %30 = xor i64 %29, %28
  store i64 %30, ptr %element10, align 1
  %address879 = getelementptr i8, ptr %5, i64 24
  %address880 = getelementptr i8, ptr %address879, i64 %17
  %element717 = getelementptr i64, ptr %address880, i64 %r9.0
  %31 = load i64, ptr %element717, align 1
  %element11 = getelementptr i64, ptr %22, i64 -134
  %32 = load i64, ptr %element11, align 1
  %33 = xor i64 %32, %31
  store i64 %33, ptr %element11, align 1
  %34 = add i64 %r9.0, 4
  %c426.not = icmp eq i64 %34, 128
  br i1 %c426.not, label %bb_140026670, label %bb_1400266c0

bb_140026710:
  br label %bb_140026720

bb_140026720:
  %r9.1 = phi i64 [ 0, %bb_140026710 ], [ %48, %bb_140026720 ]
  %sc457 = shl i64 %r9.1, 3
  %address887 = getelementptr i8, ptr %5, i64 %17
  %address684 = getelementptr i8, ptr %address887, i64 %sc457
  %35 = load i128, ptr %address684, align 1
  %element12 = getelementptr i128, ptr %address684, i64 1
  %36 = load i128, ptr %element12, align 1
  %37 = getelementptr i8, ptr %sp0, i64 %sc457
  %fp468 = getelementptr i8, ptr %37, i64 -1096
  %38 = load i128, ptr %fp468, align 1
  %39 = xor i128 %38, %35
  %fp473 = getelementptr i8, ptr %37, i64 -1080
  %40 = load i128, ptr %fp473, align 1
  %41 = xor i128 %40, %36
  %fp479 = getelementptr i8, ptr %37, i64 -1064
  %42 = load i128, ptr %fp479, align 1
  %fp483 = getelementptr i8, ptr %37, i64 -1048
  %43 = load i128, ptr %fp483, align 1
  store i128 %39, ptr %fp468, align 1
  store i128 %41, ptr %fp473, align 1
  %element13 = getelementptr i128, ptr %address684, i64 2
  %44 = load i128, ptr %element13, align 1
  %45 = xor i128 %44, %42
  %address895 = getelementptr i8, ptr %5, i64 48
  %address896 = getelementptr i8, ptr %address895, i64 %17
  %address693 = getelementptr i8, ptr %address896, i64 %sc457
  %46 = load i128, ptr %address693, align 1
  %47 = xor i128 %46, %43
  store i128 %45, ptr %fp479, align 1
  store i128 %47, ptr %fp483, align 1
  %48 = add i64 %r9.1, 8
  %c545.not = icmp eq i64 %48, 128
  br i1 %c545.not, label %bb_140026670, label %bb_140026720

bb_14002679e:
  %fp546 = getelementptr inbounds nuw i8, ptr %frame, i64 40
  %49 = ptrtoint ptr %fp546 to i64
  %50 = call i64 @sub_1400AD000(ptr nonnull %fp546, ptr nonnull %fp104, i64 1024)
  %element14 = getelementptr i32, ptr %0, i64 2
  %51 = load i32, ptr %element14, align 1
  %z556 = zext i32 %51 to i64
  %52 = load ptr, ptr %0, align 1
  %53 = call i32 @sub_14002A290(ptr %52, i64 %z556, i64 %49, i64 1024)
  %54 = call i64 @sub_14001C970(ptr nonnull %fp104, i64 1024)
  %55 = call i64 @sub_14001C970(ptr nonnull %fp546, i64 1024)
  %element15 = getelementptr i32, ptr %0, i64 23
  %56 = load i32, ptr %element15, align 1
  %e578 = trunc i32 %56 to i8
  %57 = call i64 @sub_140026840(ptr nonnull %1, i8 %e578)
  %.pre = load i64, ptr %fp41, align 1
  %58 = xor i64 %.pre, %rsp32
  br label %bb_1400267f5

bb_1400267f5:
  %59 = phi i64 [ %58, %bb_14002679e ], [ 47936899621426, %bb_1400265c0 ]
  %c605.not = icmp eq i64 %59, 47936899621426
  br i1 %c605.not, label %bb_14002680c, label %bb_140026820

bb_14002680c:
  ret i64 47936899621426

bb_140026820:
  call void @sub_14003F9F0(i64 %59)
  call void @llvm.x86.int(i8 3)
  unreachable
}
