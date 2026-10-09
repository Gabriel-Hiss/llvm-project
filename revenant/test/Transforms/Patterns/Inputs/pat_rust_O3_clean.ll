; ModuleID = 'C:/Users/User/RustroverProjects/Revenant/target/pattern-snippets/pat_rust_O3.ll'
source_filename = "pat.b2e2e07ff26e79e5-cgu.0"
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

@alloc_5726901dfd27ca506e7b9ded8454b614 = private unnamed_addr constant [5 x i8] c"\02n=\C0\00", align 1
@alloc_77a757558ef92a3cfef240856bb20017 = private unnamed_addr constant [72 x i8] c"C:\\Users\\User\\RustroverProjects\\Revenant\\target\\pattern-snippets\\pat.rs\00", align 1
@alloc_1414fb96e41d03c5f56df2e9ff979d54 = private unnamed_addr constant <{ ptr, [16 x i8] }> <{ ptr @alloc_77a757558ef92a3cfef240856bb20017, [16 x i8] c"G\00\00\00\00\00\00\00\04\00\00\00I\00\00\00" }>, align 8
@alloc_f2fa1c869d12a8f96bbba17779984dad = private unnamed_addr constant <{ ptr, [16 x i8] }> <{ ptr @alloc_77a757558ef92a3cfef240856bb20017, [16 x i8] c"G\00\00\00\00\00\00\00\05\00\00\00G\00\00\00" }>, align 8
@alloc_188df040635fc649b165eadf3f36bd3e = private unnamed_addr constant <{ ptr, [16 x i8] }> <{ ptr @alloc_77a757558ef92a3cfef240856bb20017, [16 x i8] c"G\00\00\00\00\00\00\00\06\00\00\00F\00\00\00" }>, align 8

; Function Attrs: noinline uwtable
define void @_RNvCsfmcQEXjCgZZ_3pat10pat_format(ptr dead_on_unwind noalias noundef writable sret([24 x i8]) align 8 captures(none) dereferenceable(24) %_0, i64 noundef %0) unnamed_addr #0 personality ptr @__CxxFrameHandler3 {
start:
  %args = alloca [16 x i8], align 8
  %n = alloca [8 x i8], align 8
  store i64 %0, ptr %n, align 8
  call void @llvm.lifetime.start.p0(ptr nonnull %args)
  store ptr %n, ptr %args, align 8
  %_5.sroa.4.0..sroa_idx = getelementptr inbounds nuw i8, ptr %args, i64 8
  store ptr @_RNvXse_NtNtNtCsd9eshbB3hfz_4core3fmt3num3impxNtB9_7Display3fmt, ptr %_5.sroa.4.0..sroa_idx, align 8
  call void @_RNvNvNtCsdkPGTeTFVes_5alloc3fmt6format12format_inner(ptr noalias noundef nonnull sret([24 x i8]) align 8 captures(none) dereferenceable(24) %_0, ptr noundef nonnull @alloc_5726901dfd27ca506e7b9ded8454b614, ptr noundef nonnull %args)
  call void @llvm.lifetime.end.p0(ptr nonnull %args)
  ret void
}

; Function Attrs: nofree noinline norecurse nosync nounwind memory(argmem: read) uwtable
define noundef i64 @_RNvCsfmcQEXjCgZZ_3pat12pat_iter_sum(ptr noalias noundef nonnull readonly align 8 captures(none) %s.0, i64 noundef range(i64 0, 1152921504606846976) %s.1) unnamed_addr #1 personality ptr @__CxxFrameHandler3 {
start:
  %0 = icmp eq i64 %s.1, 0
  br i1 %0, label %_RINvXs2J_NtNtCsd9eshbB3hfz_4core5slice4iterINtB7_4IterxENtNtNtNtBb_4iter6traits8iterator8Iterator4foldxNCINvNtNtBY_8adapters3map8map_foldRxxxNCNvCsfmcQEXjCgZZ_3pat12pat_iter_sum0NCINvXse_NtBW_5accumxNtB2Z_3Sum3sumINtB1I_3MapBF_B2f_EE0E0EB2j_.exit, label %bb8.i.preheader

bb8.i.preheader:                                  ; preds = %start
  %min.iters.check = icmp samesign ult i64 %s.1, 4
  br i1 %min.iters.check, label %bb8.i.preheader3, label %vector.ph

vector.ph:                                        ; preds = %bb8.i.preheader
  %n.vec = and i64 %s.1, 1152921504606846972
  br label %vector.body

vector.body:                                      ; preds = %vector.body, %vector.ph
  %index = phi i64 [ 0, %vector.ph ], [ %index.next, %vector.body ]
  %vec.phi = phi <2 x i64> [ zeroinitializer, %vector.ph ], [ %5, %vector.body ]
  %vec.phi1 = phi <2 x i64> [ zeroinitializer, %vector.ph ], [ %6, %vector.body ]
  %1 = getelementptr inbounds nuw i64, ptr %s.0, i64 %index
  %2 = getelementptr inbounds nuw i8, ptr %1, i64 16
  %wide.load = load <2 x i64>, ptr %1, align 8
  %wide.load2 = load <2 x i64>, ptr %2, align 8
  %3 = shl <2 x i64> %wide.load, splat (i64 1)
  %4 = shl <2 x i64> %wide.load2, splat (i64 1)
  %5 = add <2 x i64> %3, %vec.phi
  %6 = add <2 x i64> %4, %vec.phi1
  %index.next = add nuw i64 %index, 4
  %7 = icmp eq i64 %index.next, %n.vec
  br i1 %7, label %middle.block, label %vector.body, !llvm.loop !2

middle.block:                                     ; preds = %vector.body
  %bin.rdx = add <2 x i64> %6, %5
  %8 = tail call i64 @llvm.vector.reduce.add.v2i64(<2 x i64> %bin.rdx)
  %cmp.n = icmp eq i64 %s.1, %n.vec
  br i1 %cmp.n, label %_RINvXs2J_NtNtCsd9eshbB3hfz_4core5slice4iterINtB7_4IterxENtNtNtNtBb_4iter6traits8iterator8Iterator4foldxNCINvNtNtBY_8adapters3map8map_foldRxxxNCNvCsfmcQEXjCgZZ_3pat12pat_iter_sum0NCINvXse_NtBW_5accumxNtB2Z_3Sum3sumINtB1I_3MapBF_B2f_EE0E0EB2j_.exit, label %bb8.i.preheader3

bb8.i.preheader3:                                 ; preds = %middle.block, %bb8.i.preheader
  %i.sroa.0.0.i.ph = phi i64 [ 0, %bb8.i.preheader ], [ %n.vec, %middle.block ]
  %acc.sroa.0.0.i.ph = phi i64 [ 0, %bb8.i.preheader ], [ %8, %middle.block ]
  br label %bb8.i

bb8.i:                                            ; preds = %bb8.i, %bb8.i.preheader3
  %i.sroa.0.0.i = phi i64 [ %_27.i, %bb8.i ], [ %i.sroa.0.0.i.ph, %bb8.i.preheader3 ]
  %acc.sroa.0.0.i = phi i64 [ %_4.0.i1.i.i, %bb8.i ], [ %acc.sroa.0.0.i.ph, %bb8.i.preheader3 ]
  %_45.i = getelementptr inbounds nuw i64, ptr %s.0, i64 %i.sroa.0.0.i
  %_45.val.i = load i64, ptr %_45.i, align 8, !noundef !5
  %_4.0.i.i.i = shl i64 %_45.val.i, 1
  %_4.0.i1.i.i = add i64 %_4.0.i.i.i, %acc.sroa.0.0.i
  %_27.i = add nuw nsw i64 %i.sroa.0.0.i, 1
  %_28.i = icmp eq i64 %_27.i, %s.1
  br i1 %_28.i, label %_RINvXs2J_NtNtCsd9eshbB3hfz_4core5slice4iterINtB7_4IterxENtNtNtNtBb_4iter6traits8iterator8Iterator4foldxNCINvNtNtBY_8adapters3map8map_foldRxxxNCNvCsfmcQEXjCgZZ_3pat12pat_iter_sum0NCINvXse_NtBW_5accumxNtB2Z_3Sum3sumINtB1I_3MapBF_B2f_EE0E0EB2j_.exit, label %bb8.i, !llvm.loop !6

_RINvXs2J_NtNtCsd9eshbB3hfz_4core5slice4iterINtB7_4IterxENtNtNtNtBb_4iter6traits8iterator8Iterator4foldxNCINvNtNtBY_8adapters3map8map_foldRxxxNCNvCsfmcQEXjCgZZ_3pat12pat_iter_sum0NCINvXse_NtBW_5accumxNtB2Z_3Sum3sumINtB1I_3MapBF_B2f_EE0E0EB2j_.exit: ; preds = %bb8.i, %middle.block, %start
  %_0.sroa.0.0.i = phi i64 [ 0, %start ], [ %8, %middle.block ], [ %_4.0.i1.i.i, %bb8.i ]
  ret i64 %_0.sroa.0.0.i
}

; Function Attrs: noinline uwtable
define void @_RNvCsfmcQEXjCgZZ_3pat12pat_vec_push(ptr noalias noundef align 8 captures(none) dereferenceable(24) %v, i64 noundef %x) unnamed_addr #0 personality ptr @__CxxFrameHandler3 {
start:
  tail call void @llvm.experimental.noalias.scope.decl(metadata !7)
  %0 = getelementptr inbounds nuw i8, ptr %v, i64 16
  %len.i = load i64, ptr %0, align 8, !alias.scope !7, !noundef !5
  %self1.i = load i64, ptr %v, align 8, !range !10, !alias.scope !7, !noundef !5
  %_4.i = icmp eq i64 %len.i, %self1.i
  br i1 %_4.i, label %bb1.i, label %_RNvMsF_NtCsdkPGTeTFVes_5alloc3vecINtB5_3VecxE8push_mutCsfmcQEXjCgZZ_3pat.exit

bb1.i:                                            ; preds = %start
  tail call void @_RNvMs3_NtCsdkPGTeTFVes_5alloc7raw_vecINtB5_6RawVecxE8grow_oneCsfmcQEXjCgZZ_3pat(ptr noalias noundef nonnull align 8 dereferenceable(24) %v)
  br label %_RNvMsF_NtCsdkPGTeTFVes_5alloc3vecINtB5_3VecxE8push_mutCsfmcQEXjCgZZ_3pat.exit

_RNvMsF_NtCsdkPGTeTFVes_5alloc3vecINtB5_3VecxE8push_mutCsfmcQEXjCgZZ_3pat.exit: ; preds = %bb1.i, %start
  %1 = getelementptr inbounds nuw i8, ptr %v, i64 8
  %_14.i = load ptr, ptr %1, align 8, !alias.scope !7, !nonnull !5, !noundef !5
  %end.i = getelementptr inbounds nuw i64, ptr %_14.i, i64 %len.i
  store i64 %x, ptr %end.i, align 8, !noalias !7
  %2 = add i64 %len.i, 1
  store i64 %2, ptr %0, align 8, !alias.scope !7
  ret void
}

; Function Attrs: noinline uwtable
define noundef i64 @_RNvCsfmcQEXjCgZZ_3pat13pat_vec_index(ptr noalias noundef readonly align 8 captures(none) dereferenceable(24) %v, i64 noundef %i) unnamed_addr #0 {
start:
  %0 = getelementptr inbounds nuw i8, ptr %v, i64 16
  %_6 = load i64, ptr %0, align 8, !noundef !5
  %_8 = icmp ult i64 %i, %_6
  br i1 %_8, label %bb1, label %panic

bb1:                                              ; preds = %start
  %1 = getelementptr inbounds nuw i8, ptr %v, i64 8
  %_7 = load ptr, ptr %1, align 8, !nonnull !5, !noundef !5
  %_3 = getelementptr inbounds nuw i64, ptr %_7, i64 %i
  %_0 = load i64, ptr %_3, align 8, !noundef !5
  ret i64 %_0

panic:                                            ; preds = %start
  tail call void @_RNvNtCsd9eshbB3hfz_4core9panicking18panic_bounds_check(i64 noundef %i, i64 noundef %_6, ptr noalias noundef readonly align 8 captures(address, read_provenance) dereferenceable(24) @alloc_1414fb96e41d03c5f56df2e9ff979d54) #18
  unreachable
}

; Function Attrs: noinline uwtable
define noundef i64 @_RNvCsfmcQEXjCgZZ_3pat15pat_slice_index(ptr noalias noundef nonnull readonly align 8 captures(none) %s.0, i64 noundef range(i64 0, 1152921504606846976) %s.1, i64 noundef %i) unnamed_addr #0 {
start:
  %_4 = icmp ult i64 %i, %s.1
  br i1 %_4, label %bb1, label %panic

bb1:                                              ; preds = %start
  %0 = getelementptr inbounds nuw i64, ptr %s.0, i64 %i
  %_0 = load i64, ptr %0, align 8, !noundef !5
  ret i64 %_0

panic:                                            ; preds = %start
  tail call void @_RNvNtCsd9eshbB3hfz_4core9panicking18panic_bounds_check(i64 noundef %i, i64 noundef %s.1, ptr noalias noundef readonly align 8 captures(address, read_provenance) dereferenceable(24) @alloc_f2fa1c869d12a8f96bbba17779984dad) #18
  unreachable
}

; Function Attrs: noinline uwtable
define void @_RNvCsfmcQEXjCgZZ_3pat15pat_string_from(ptr dead_on_unwind noalias noundef writable writeonly sret([24 x i8]) align 8 captures(none) dereferenceable(24) %_0, ptr noalias noundef nonnull readonly captures(none) %s.0, i64 noundef %s.1) unnamed_addr #0 personality ptr @__CxxFrameHandler3 {
start:
  %_39.i = icmp sgt i64 %s.1, -1
  br i1 %_39.i, label %bb18.i, label %bb5

bb18.i:                                           ; preds = %start
  %0 = icmp eq i64 %s.1, 0
  br i1 %0, label %bb2, label %_RNvXs_NtCsdkPGTeTFVes_5alloc5allocNtB4_6GlobalNtNtCsd9eshbB3hfz_4core5alloc9Allocator8allocate.exit.i

_RNvXs_NtCsdkPGTeTFVes_5alloc5allocNtB4_6GlobalNtNtCsd9eshbB3hfz_4core5alloc9Allocator8allocate.exit.i: ; preds = %bb18.i
  tail call void @_RNvCs9hJ03s5DiqP_7___rustc35___rust_no_alloc_shim_is_unstable_v2() #19, !noalias !11
  %1 = tail call noundef ptr @_RNvCs9hJ03s5DiqP_7___rustc12___rust_alloc(i64 noundef range(i64 0, -9223372036854775808) %s.1, i64 noundef range(i64 1, 9) 1) #19, !noalias !11
  %2 = icmp eq ptr %1, null
  br i1 %2, label %bb5, label %bb1

bb5:                                              ; preds = %_RNvXs_NtCsdkPGTeTFVes_5alloc5allocNtB4_6GlobalNtNtCsd9eshbB3hfz_4core5alloc9Allocator8allocate.exit.i, %start
  %_12.sroa.4.0.ph = phi i64 [ 1, %_RNvXs_NtCsdkPGTeTFVes_5alloc5allocNtB4_6GlobalNtNtCsd9eshbB3hfz_4core5alloc9Allocator8allocate.exit.i ], [ 0, %start ]
  tail call void @_RNvNtCsdkPGTeTFVes_5alloc7raw_vec12handle_error(i64 noundef %_12.sroa.4.0.ph, i64 %s.1) #18
  unreachable

bb2:                                              ; preds = %bb1, %bb18.i
  %3 = phi ptr [ %1, %bb1 ], [ inttoptr (i64 1 to ptr), %bb18.i ]
  store i64 %s.1, ptr %_0, align 8
  %_2.sroa.4.0._0.sroa_idx = getelementptr inbounds nuw i8, ptr %_0, i64 8
  store ptr %3, ptr %_2.sroa.4.0._0.sroa_idx, align 8
  %_2.sroa.6.0._0.sroa_idx = getelementptr inbounds nuw i8, ptr %_0, i64 16
  store i64 %s.1, ptr %_2.sroa.6.0._0.sroa_idx, align 8
  ret void

bb1:                                              ; preds = %_RNvXs_NtCsdkPGTeTFVes_5alloc5allocNtB4_6GlobalNtNtCsd9eshbB3hfz_4core5alloc9Allocator8allocate.exit.i
  tail call void @llvm.memcpy.p0.p0.i64(ptr nonnull align 1 %1, ptr nonnull align 1 %s.0, i64 %s.1, i1 false)
  br label %bb2
}

; Function Attrs: noinline uwtable
define noundef i64 @_RNvCsfmcQEXjCgZZ_3pat17pat_option_unwrap(i64 noundef range(i64 0, 2) %0, i64 returned %1) unnamed_addr #0 {
start:
  %2 = trunc nuw i64 %0 to i1
  br i1 %2, label %bb3, label %bb2, !prof !14

bb3:                                              ; preds = %start
  ret i64 %1

bb2:                                              ; preds = %start
  tail call void @_RNvNtCsd9eshbB3hfz_4core6option13unwrap_failed(ptr noalias noundef readonly align 8 captures(address, read_provenance) dereferenceable(24) @alloc_188df040635fc649b165eadf3f36bd3e) #18
  unreachable
}

; Function Attrs: cold noinline uwtable
define void @_RNvMs3_NtCsdkPGTeTFVes_5alloc7raw_vecINtB5_6RawVecxE8grow_oneCsfmcQEXjCgZZ_3pat(ptr noalias noundef align 8 captures(none) dereferenceable(16) %self) unnamed_addr #2 {
start:
  %self1 = load i64, ptr %self, align 8, !range !10, !noundef !5
  %0 = tail call fastcc { i64, i64 } @_RNvMs4_NtCsdkPGTeTFVes_5alloc7raw_vecNtB5_11RawVecInner14grow_amortizedCsfmcQEXjCgZZ_3pat(ptr noalias noundef align 8 dereferenceable(16) %self, i64 noundef %self1)
  %1 = extractvalue { i64, i64 } %0, 0
  %.not = icmp eq i64 %1, -1
  br i1 %.not, label %bb3, label %bb2, !prof !14

bb2:                                              ; preds = %start
  %2 = extractvalue { i64, i64 } %0, 1
  tail call void @_RNvNtCsdkPGTeTFVes_5alloc7raw_vec12handle_error(i64 noundef %1, i64 %2) #18
  unreachable

bb3:                                              ; preds = %start
  ret void
}

; Function Attrs: cold nounwind uwtable
define internal fastcc void @_RNvMs4_NtCsdkPGTeTFVes_5alloc7raw_vecNtB5_11RawVecInner11finish_growCsfmcQEXjCgZZ_3pat(ptr dead_on_unwind noalias noundef nonnull writable writeonly align 8 captures(none) dereferenceable(24) initializes((0, 8)) %_0, i64 %self.0.val, ptr %self.8.val, i64 noundef range(i64 0, -1) %cap) unnamed_addr #3 {
start:
  %_34.0 = shl i64 %cap, 3
  %_34.1 = icmp ult i64 %cap, 2305843009213693952
  %_39 = icmp ult i64 %_34.0, 9223372036854775801
  %or.cond = and i1 %_34.1, %_39
  br i1 %or.cond, label %bb15, label %bb11, !prof !15

bb15:                                             ; preds = %start
  %0 = icmp eq i64 %self.0.val, 0
  br i1 %0, label %bb5, label %bb3

bb3:                                              ; preds = %bb15
  %alloc_size.i = shl nuw i64 %self.0.val, 3
  %1 = icmp ne ptr %self.8.val, null
  tail call void @llvm.assume(i1 %1)
  %cond.i.i = icmp uge i64 %_34.0, %alloc_size.i
  tail call void @llvm.assume(i1 %cond.i.i)
  %raw_ptr.i.i = tail call noundef align 8 ptr @_RNvCs9hJ03s5DiqP_7___rustc14___rust_realloc(ptr noundef nonnull %self.8.val, i64 noundef %alloc_size.i, i64 noundef 8, i64 noundef range(i64 0, 9223372036854775801) %_34.0) #19
  br label %bb7

bb5:                                              ; preds = %bb15
  %2 = icmp eq i64 %_34.0, 0
  br i1 %2, label %bb9, label %bb4.i.i

bb4.i.i:                                          ; preds = %bb5
  tail call void @_RNvCs9hJ03s5DiqP_7___rustc35___rust_no_alloc_shim_is_unstable_v2() #19
  %3 = tail call noundef align 8 ptr @_RNvCs9hJ03s5DiqP_7___rustc12___rust_alloc(i64 noundef range(i64 0, -9223372036854775808) %_34.0, i64 noundef range(i64 1, 9) 8) #19
  br label %bb7

bb7:                                              ; preds = %bb4.i.i, %bb3
  %raw_ptr.i.i.pn = phi ptr [ %raw_ptr.i.i, %bb3 ], [ %3, %bb4.i.i ]
  %4 = icmp eq ptr %raw_ptr.i.i.pn, null
  br i1 %4, label %bb8, label %bb9

bb8:                                              ; preds = %bb7
  %5 = getelementptr inbounds nuw i8, ptr %_0, i64 8
  store i64 8, ptr %5, align 8
  br label %bb11

bb9:                                              ; preds = %bb7, %bb5
  %raw_ptr.i.i.pn8 = phi ptr [ %raw_ptr.i.i.pn, %bb7 ], [ inttoptr (i64 8 to ptr), %bb5 ]
  %6 = getelementptr inbounds nuw i8, ptr %_0, i64 8
  store ptr %raw_ptr.i.i.pn8, ptr %6, align 8
  br label %bb11

bb11:                                             ; preds = %bb9, %bb8, %start
  %.sink9 = phi i64 [ 16, %bb9 ], [ 16, %bb8 ], [ 8, %start ]
  %_34.0.sink = phi i64 [ %_34.0, %bb9 ], [ %_34.0, %bb8 ], [ 0, %start ]
  %storemerge9 = phi i64 [ 0, %bb9 ], [ 1, %bb8 ], [ 1, %start ]
  %7 = getelementptr inbounds nuw i8, ptr %_0, i64 %.sink9
  store i64 %_34.0.sink, ptr %7, align 8
  store i64 %storemerge9, ptr %_0, align 8
  ret void
}

; Function Attrs: cold nounwind uwtable
define internal fastcc { i64, i64 } @_RNvMs4_NtCsdkPGTeTFVes_5alloc7raw_vecNtB5_11RawVecInner14grow_amortizedCsfmcQEXjCgZZ_3pat(ptr noalias noundef nonnull align 8 captures(none) dereferenceable(16) %self, i64 noundef range(i64 0, -9223372036854775808) %len) unnamed_addr #3 personality ptr @__CxxFrameHandler3 {
start:
  %self3 = alloca [24 x i8], align 8
  %_25.0 = add nuw i64 %len, 1
  %self5 = load i64, ptr %self, align 8, !range !10, !noundef !5
  %v16 = shl nuw i64 %self5, 1
  %..i = tail call noundef range(i64 0, -1) i64 @llvm.umax.i64(i64 range(i64 0, -1) %_25.0, i64 range(i64 0, -1) %v16)
  %..i16 = tail call noundef range(i64 0, -1) i64 @llvm.umax.i64(i64 range(i64 0, -1) %..i, i64 4)
  call void @llvm.lifetime.start.p0(ptr nonnull %self3)
  %0 = getelementptr inbounds nuw i8, ptr %self, i64 8
  %self.val15 = load ptr, ptr %0, align 8
  call fastcc void @_RNvMs4_NtCsdkPGTeTFVes_5alloc7raw_vecNtB5_11RawVecInner11finish_growCsfmcQEXjCgZZ_3pat(ptr noalias noundef align 8 captures(none) dereferenceable(24) %self3, i64 %self5, ptr %self.val15, i64 noundef %..i16)
  %_37 = load i64, ptr %self3, align 8, !range !16, !noundef !5
  %1 = trunc nuw i64 %_37 to i1
  %2 = getelementptr inbounds nuw i8, ptr %self3, i64 8
  br i1 %1, label %bb18, label %bb19

bb6:                                              ; preds = %bb19, %bb18
  %_0.sroa.5.0 = phi i64 [ undef, %bb19 ], [ %e.1, %bb18 ]
  %_0.sroa.0.0 = phi i64 [ -1, %bb19 ], [ %e.0, %bb18 ]
  %3 = insertvalue { i64, i64 } poison, i64 %_0.sroa.0.0, 0
  %4 = insertvalue { i64, i64 } %3, i64 %_0.sroa.5.0, 1
  ret { i64, i64 } %4

bb18:                                             ; preds = %start
  %e.0 = load i64, ptr %2, align 8, !range !17, !noundef !5
  %5 = getelementptr inbounds nuw i8, ptr %self3, i64 16
  %e.1 = load i64, ptr %5, align 8
  call void @llvm.lifetime.end.p0(ptr nonnull %self3)
  br label %bb6

bb19:                                             ; preds = %start
  %v.0 = load ptr, ptr %2, align 8, !nonnull !5, !noundef !5
  call void @llvm.lifetime.end.p0(ptr nonnull %self3)
  store ptr %v.0, ptr %0, align 8
  %6 = icmp sgt i64 %..i16, -1
  tail call void @llvm.assume(i1 %6)
  store i64 %..i16, ptr %self, align 8
  br label %bb6
}

declare i32 @__CxxFrameHandler3(...) unnamed_addr #4

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.start.p0(ptr captures(none)) #5

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(inaccessiblemem: write)
declare void @llvm.assume(i1 noundef) #6

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.end.p0(ptr captures(none)) #5

; Function Attrs: nocallback nofree nounwind willreturn memory(argmem: readwrite)
declare void @llvm.memcpy.p0.p0.i64(ptr noalias writeonly captures(none), ptr noalias readonly captures(none), i64, i1 immarg) #7

; Function Attrs: uwtable
declare void @_RNvNvNtCsdkPGTeTFVes_5alloc3fmt6format12format_inner(ptr dead_on_unwind noalias noundef writable sret([24 x i8]) align 8 captures(none) dereferenceable(24), ptr noundef nonnull, ptr noundef nonnull) unnamed_addr #8

; Function Attrs: uwtable
declare noundef zeroext i1 @_RNvXse_NtNtNtCsd9eshbB3hfz_4core3fmt3num3impxNtB9_7Display3fmt(ptr noalias noundef readonly align 8 captures(address, read_provenance) dereferenceable(8), ptr noalias noundef align 8 dereferenceable(24)) unnamed_addr #8

; Function Attrs: cold minsize noinline noreturn optsize uwtable
declare void @_RNvNtCsd9eshbB3hfz_4core9panicking18panic_bounds_check(i64 noundef, i64 noundef, ptr noalias noundef readonly align 8 captures(address, read_provenance) dereferenceable(24)) unnamed_addr #9

; Function Attrs: cold minsize noreturn optsize uwtable
declare void @_RNvNtCsdkPGTeTFVes_5alloc7raw_vec12handle_error(i64 noundef range(i64 0, -9223372036854775807), i64) unnamed_addr #10

; Function Attrs: cold noinline noreturn uwtable
declare void @_RNvNtCsd9eshbB3hfz_4core6option13unwrap_failed(ptr noalias noundef readonly align 8 captures(address, read_provenance) dereferenceable(24)) unnamed_addr #11

; Function Attrs: nounwind allockind("realloc,aligned") allocsize(3) uwtable
declare noalias noundef ptr @_RNvCs9hJ03s5DiqP_7___rustc14___rust_realloc(ptr allocptr noundef nonnull, i64 noundef, i64 allocalign noundef range(i64 1, -9223372036854775807), i64 noundef) unnamed_addr #12

; Function Attrs: nounwind uwtable
declare void @_RNvCs9hJ03s5DiqP_7___rustc35___rust_no_alloc_shim_is_unstable_v2() unnamed_addr #13

; Function Attrs: nounwind allockind("alloc,uninitialized,aligned") allocsize(0) uwtable
declare noalias noundef ptr @_RNvCs9hJ03s5DiqP_7___rustc12___rust_alloc(i64 noundef, i64 allocalign noundef range(i64 1, -9223372036854775807)) unnamed_addr #14

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(inaccessiblemem: readwrite)
declare void @llvm.experimental.noalias.scope.decl(metadata) #15

; Function Attrs: nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none)
declare i64 @llvm.umax.i64(i64, i64) #16

; Function Attrs: nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare i64 @llvm.vector.reduce.add.v2i64(<2 x i64>) #17

attributes #0 = { noinline uwtable "target-cpu"="x86-64" "target-features"="+cx16,+sse,+sse2,+sse3,+sahf" }
attributes #1 = { nofree noinline norecurse nosync nounwind memory(argmem: read) uwtable "target-cpu"="x86-64" "target-features"="+cx16,+sse,+sse2,+sse3,+sahf" }
attributes #2 = { cold noinline uwtable "target-cpu"="x86-64" "target-features"="+cx16,+sse,+sse2,+sse3,+sahf" }
attributes #3 = { cold nounwind uwtable "target-cpu"="x86-64" "target-features"="+cx16,+sse,+sse2,+sse3,+sahf" }
attributes #4 = { "target-cpu"="x86-64" }
attributes #5 = { nocallback nofree nosync nounwind willreturn memory(argmem: readwrite) }
attributes #6 = { nocallback nofree nosync nounwind willreturn memory(inaccessiblemem: write) }
attributes #7 = { nocallback nofree nounwind willreturn memory(argmem: readwrite) }
attributes #8 = { uwtable "target-cpu"="x86-64" "target-features"="+cx16,+sse,+sse2,+sse3,+sahf" }
attributes #9 = { cold minsize noinline noreturn optsize uwtable "target-cpu"="x86-64" "target-features"="+cx16,+sse,+sse2,+sse3,+sahf" }
attributes #10 = { cold minsize noreturn optsize uwtable "target-cpu"="x86-64" "target-features"="+cx16,+sse,+sse2,+sse3,+sahf" }
attributes #11 = { cold noinline noreturn uwtable "target-cpu"="x86-64" "target-features"="+cx16,+sse,+sse2,+sse3,+sahf" }
attributes #12 = { nounwind allockind("realloc,aligned") allocsize(3) uwtable "alloc-family"="__rust_alloc" "target-cpu"="x86-64" "target-features"="+cx16,+sse,+sse2,+sse3,+sahf" }
attributes #13 = { nounwind uwtable "target-cpu"="x86-64" "target-features"="+cx16,+sse,+sse2,+sse3,+sahf" }
attributes #14 = { nounwind allockind("alloc,uninitialized,aligned") allocsize(0) uwtable "alloc-family"="__rust_alloc" "alloc-variant-zeroed"="_RNvCs9hJ03s5DiqP_7___rustc19___rust_alloc_zeroed" "target-cpu"="x86-64" "target-features"="+cx16,+sse,+sse2,+sse3,+sahf" }
attributes #15 = { nocallback nofree nosync nounwind willreturn memory(inaccessiblemem: readwrite) }
attributes #16 = { nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none) }
attributes #17 = { nocallback nofree nosync nounwind speculatable willreturn memory(none) }
attributes #18 = { noreturn }
attributes #19 = { nounwind }

!llvm.module.flags = !{!0}
!llvm.ident = !{!1}

!0 = !{i32 8, !"PIC Level", i32 2}
!1 = !{!"rustc version 1.97.0 (2d8144b78 2026-07-07)"}
!2 = distinct !{!2, !3, !4}
!3 = !{!"llvm.loop.isvectorized", i32 1}
!4 = !{!"llvm.loop.unroll.runtime.disable"}
!5 = !{}
!6 = distinct !{!6, !4, !3}
!7 = !{!8}
!8 = distinct !{!8, !9, !"_RNvMsF_NtCsdkPGTeTFVes_5alloc3vecINtB5_3VecxE8push_mutCsfmcQEXjCgZZ_3pat: %self"}
!9 = distinct !{!9, !"_RNvMsF_NtCsdkPGTeTFVes_5alloc3vecINtB5_3VecxE8push_mutCsfmcQEXjCgZZ_3pat"}
!10 = !{i64 0, i64 -9223372036854775808}
!11 = !{!12}
!12 = distinct !{!12, !13, !"_RNvMs4_NtCsdkPGTeTFVes_5alloc7raw_vecNtB5_11RawVecInner15try_allocate_inCsfmcQEXjCgZZ_3pat: %_0"}
!13 = distinct !{!13, !"_RNvMs4_NtCsdkPGTeTFVes_5alloc7raw_vecNtB5_11RawVecInner15try_allocate_inCsfmcQEXjCgZZ_3pat"}
!14 = !{!"branch_weights", !"expected", i32 2000, i32 1}
!15 = !{!"branch_weights", i32 2000, i32 2002}
!16 = !{i64 0, i64 2}
!17 = !{i64 0, i64 -9223372036854775807}
