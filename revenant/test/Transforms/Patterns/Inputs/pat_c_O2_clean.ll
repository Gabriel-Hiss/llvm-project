; ModuleID = 'C:/Users/User/RustroverProjects/Revenant/target/pattern-snippets/pat_c_O2.ll'
source_filename = "pat.c"
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc19.51.36248"

; Function Attrs: mustprogress nofree noinline norecurse nounwind sspstrong willreturn memory(argmem: read) uwtable
define dso_local i32 @pat_strlen(ptr noundef readonly captures(none) %0) local_unnamed_addr #0 {
  %2 = tail call i64 @strlen(ptr noundef nonnull dereferenceable(1) %0) #6
  %3 = trunc i64 %2 to i32
  ret i32 %3
}

; Function Attrs: mustprogress nocallback nofree nounwind willreturn memory(argmem: read)
declare dso_local i64 @strlen(ptr noundef captures(none)) local_unnamed_addr #1

; Function Attrs: mustprogress nofree noinline norecurse nosync nounwind sspstrong willreturn memory(argmem: write) uwtable
define dso_local void @pat_memset(ptr noundef writeonly captures(none) %0, i32 noundef %1, i32 noundef %2) local_unnamed_addr #2 {
  %4 = trunc i32 %1 to i8
  %5 = zext i32 %2 to i64
  tail call void @llvm.memset.p0.i64(ptr align 1 %0, i8 %4, i64 %5, i1 false)
  ret void
}

; Function Attrs: nocallback nofree nounwind willreturn memory(argmem: write)
declare void @llvm.memset.p0.i64(ptr writeonly captures(none), i8, i64, i1 immarg) #3

; Function Attrs: mustprogress nofree noinline norecurse nosync nounwind sspstrong willreturn memory(argmem: readwrite) uwtable
define dso_local void @pat_memcpy(ptr noundef writeonly captures(none) %0, ptr noundef readonly captures(none) %1, i32 noundef %2) local_unnamed_addr #4 {
  %4 = zext i32 %2 to i64
  tail call void @llvm.memcpy.p0.p0.i64(ptr align 1 %0, ptr align 1 %1, i64 %4, i1 false)
  ret void
}

; Function Attrs: nocallback nofree nounwind willreturn memory(argmem: readwrite)
declare void @llvm.memcpy.p0.p0.i64(ptr noalias writeonly captures(none), ptr noalias readonly captures(none), i64, i1 immarg) #5

; Function Attrs: mustprogress nofree noinline norecurse nounwind sspstrong willreturn memory(argmem: read) uwtable
define dso_local i32 @pat_memcmp(ptr noundef readonly captures(none) %0, ptr noundef readonly captures(none) %1, i32 noundef %2) local_unnamed_addr #0 {
  %4 = zext i32 %2 to i64
  %5 = tail call i32 @memcmp(ptr noundef %0, ptr noundef %1, i64 noundef %4)
  ret i32 %5
}

; Function Attrs: mustprogress nocallback nofree nounwind willreturn memory(argmem: read)
declare dso_local i32 @memcmp(ptr noundef captures(none), ptr noundef captures(none), i64 noundef) local_unnamed_addr #1

attributes #0 = { mustprogress nofree noinline norecurse nounwind sspstrong willreturn memory(argmem: read) uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #1 = { mustprogress nocallback nofree nounwind willreturn memory(argmem: read) "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #2 = { mustprogress nofree noinline norecurse nosync nounwind sspstrong willreturn memory(argmem: write) uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #3 = { nocallback nofree nounwind willreturn memory(argmem: write) }
attributes #4 = { mustprogress nofree noinline norecurse nosync nounwind sspstrong willreturn memory(argmem: readwrite) uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #5 = { nocallback nofree nounwind willreturn memory(argmem: readwrite) }
attributes #6 = { nounwind }

!llvm.dbg.cu = !{!0}
!llvm.linker.options = !{!2, !3}
!llvm.module.flags = !{!4, !5, !6, !7, !8}
!llvm.ident = !{!9}

!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, producer: "clang version 22.1.3 (https://github.com/llvm/llvm-project e9846648fd6183ee6d8cbdb4502213fcf902a211)", isOptimized: true, runtimeVersion: 0, emissionKind: NoDebug, splitDebugInlining: false, nameTableKind: None)
!1 = !DIFile(filename: "pat.c", directory: "C:\\Users\\User\\RustroverProjects\\Revenant\\target\\pattern-snippets")
!2 = !{!"/DEFAULTLIB:libcmt.lib"}
!3 = !{!"/DEFAULTLIB:oldnames.lib"}
!4 = !{i32 2, !"Debug Info Version", i32 3}
!5 = !{i32 1, !"wchar_size", i32 2}
!6 = !{i32 8, !"PIC Level", i32 2}
!7 = !{i32 7, !"uwtable", i32 2}
!8 = !{i32 1, !"MaxTLSAlign", i32 65536}
!9 = !{!"clang version 22.1.3 (https://github.com/llvm/llvm-project e9846648fd6183ee6d8cbdb4502213fcf902a211)"}
