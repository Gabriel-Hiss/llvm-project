; Hosted entry fixture: the CRT startup calls main with (argc, argv, envp).
; Real lifted IR of the c-clang-o0 user entry 0x140001DF0 (LiftFunction raw
; output), renamed to main so the emitter spells the hosted argc/argv
; signature. The body spills argv into a 64-bit frame slot as an integer,
; so the emitter must cast the pointer to the slot type.
; RUN: not a lit test; check-clang-output.py compiles the emission.
; The cited data globals are stubbed to 8 zero bytes: the body reaches them
; through offset-0 loads only, so the code under test is unchanged.

source_filename = "revenant"
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

@g_1400710c0 = internal constant [8 x i8] c"\00\00\00\00\00\00\00\00", !revenant.va !11
@g_1400710f7 = internal constant [8 x i8] c"\00\00\00\00\00\00\00\00", !revenant.va !15
@g_1400710d0 = internal constant [8 x i8] c"\00\00\00\00\00\00\00\00", !revenant.va !13
@g_140085000 = internal constant [8 x i8] c"\00\00\00\00\00\00\00\00", !revenant.va !14
@g_1400710c8 = internal constant [8 x i8] c"\00\00\00\00\00\00\00\00", !revenant.va !12
@g_140085040.2 = internal constant [8 x i8] c"\00\00\00\00\00\00\00\00", !revenant.va !4

!llvm.module.flags = !{!0}

!0 = !{i32 4, !"revenant.lang", !"c"}

!1936 = !{i64 5368717411}
!2015 = !{i64 5368717814}
!2028 = !{i64 5368717878}
!1975 = !{i64 5368717614}
!1877 = !{i64 5368717134}
!1813 = !{i64 5368716848}
!1976 = !{i64 5368717619}
!2007 = !{i64 5368717780}
!2010 = !{i64 5368717791}
!1954 = !{i64 5368717493}
!1852 = !{i64 5368717018}
!2006 = !{i64 5368717775}
!1941 = !{i64 5368717433}
!1845 = !{i64 5368716989}
!1931 = !{i64 5368717389}
!1993 = !{i64 5368717713}
!1951 = !{i64 5368717480}
!1848 = !{i64 5368717002}
!1843 = !{i64 5368716982}
!1855 = !{i64 5368717031}
!1811 = !{i64 5368716838}
!1895 = !{i64 5368717212}
!1958 = !{i64 5368717512}
!1908 = !{i64 5368717267}
!1815 = !{i64 5368716856}
!1930 = !{i64 5368717384}
!1917 = !{i64 5368717313}
!1968 = !{i64 5368717571}
!1857 = !{i64 5368717039}
!1981 = !{i64 5368717644}
!2009 = !{i64 5368717787}
!2005 = !{i64 5368717771}
!1836 = !{i64 5368716951}
!1939 = !{i64 5368717422}
!2017 = !{i64 5368717824}
!1997 = !{i64 5368717734}
!1925 = !{i64 5368717356}
!1809 = !{i64 5368716826}
!1833 = !{i64 5368716938}
!1860 = !{i64 5368717053}
!1862 = !{i64 5368717061}
!1871 = !{i64 5368717107}
!1922 = !{i64 5368717341}
!1804 = !{i64 5368716798}
!1826 = !{i64 5368716905}
!1853 = !{i64 5368717022}
!1909 = !{i64 5368717271}
!1944 = !{i64 5368717442}
!1994 = !{i64 5368717718}
!2002 = !{i64 5368717757}
!1861 = !{i64 5368717057}
!2014 = !{i64 5368717810}
!1902 = !{i64 5368717242}
!1834 = !{i64 5368716942}
!1929 = !{i64 5368717376}
!1869 = !{i64 5368717097}
!2021 = !{i64 5368717842}
!1950 = !{i64 5368717475}
!1816 = !{i64 5368716860}
!1878 = !{i64 5368717138}
!1978 = !{i64 5368717628}
!2024 = !{i64 5368717856}
!1961 = !{i64 5368717525}
!1839 = !{i64 5368716964}
!2011 = !{i64 5368717795}
!1979 = !{i64 5368717632}
!1840 = !{i64 5368716969}
!1980 = !{i64 5368717636}
!2031 = !{i64 5368717885}
!1872 = !{i64 5368717112}
!1916 = !{i64 5368717308}
!1835 = !{i64 5368716946}
!1824 = !{i64 5368716896}
!1841 = !{i64 5368716974}
!1984 = !{i64 5368717663}
!1873 = !{i64 5368717116}
!1928 = !{i64 5368717372}
!1970 = !{i64 5368717584}
!1892 = !{i64 5368717199}
!1918 = !{i64 5368717317}
!1876 = !{i64 5368717130}
!1934 = !{i64 5368717399}
!1903 = !{i64 5368717246}
!1957 = !{i64 5368717507}
!1977 = !{i64 5368717624}
!1940 = !{i64 5368717429}
!1812 = !{i64 5368716843}
!1887 = !{i64 5368717178}
!1854 = !{i64 5368717026}
!1875 = !{i64 5368717125}
!1913 = !{i64 5368717289}
!1999 = !{i64 5368717744}
!1829 = !{i64 5368716920}
!2013 = !{i64 5368717806}
!1920 = !{i64 5368717329}
!1989 = !{i64 5368717689}
!2025 = !{i64 5368717860}
!1893 = !{i64 5368717203}
!1910 = !{i64 5368717276}
!1802 = !{i64 5368716784}
!1819 = !{i64 5368716874}
!1938 = !{i64 5368717417}
!1988 = !{i64 5368717684}
!1952 = !{i64 5368717486}
!2026 = !{i64 5368717868}
!1883 = !{i64 5368717158}
!2030 = !{i64 5368717883}
!1998 = !{i64 5368717739}
!1933 = !{i64 5368717395}
!1844 = !{i64 5368716986}
!1863 = !{i64 5368717065}
!1926 = !{i64 5368717360}
!1810 = !{i64 5368716834}
!2027 = !{i64 5368717871}
!1891 = !{i64 5368717195}
!1915 = !{i64 5368717302}
!1921 = !{i64 5368717337}
!2032 = !{i64 5368717893}
!1880 = !{i64 5368717148}
!2001 = !{i64 5368717752}
!2004 = !{i64 5368717767}
!1838 = !{i64 5368716960}
!1827 = !{i64 5368716910}
!1942 = !{i64 5368717436}
!1964 = !{i64 5368717547}
!1965 = !{i64 5368717552}
!1983 = !{i64 5368717658}
!1960 = !{i64 5368717521}
!1851 = !{i64 5368717014}
!1904 = !{i64 5368717250}
!1945 = !{i64 5368717450}
!2016 = !{i64 5368717820}
!1959 = !{i64 5368717517}
!1881 = !{i64 5368717152}
!1911 = !{i64 5368717281}
!1919 = !{i64 5368717321}
!1986 = !{i64 5368717671}
!1896 = !{i64 5368717216}
!1866 = !{i64 5368717078}
!2012 = !{i64 5368717802}
!2034 = !{i64 5368717901}
!1912 = !{i64 5368717285}
!1858 = !{i64 5368717043}
!1822 = !{i64 5368716887}
!1825 = !{i64 5368716900}
!1935 = !{i64 5368717404}
!1946 = !{i64 5368717455}
!1831 = !{i64 5368716928}
!1868 = !{i64 5368717090}
!1899 = !{i64 5368717229}
!1882 = !{i64 5368717156}
!1905 = !{i64 5368717254}
!1818 = !{i64 5368716870}
!1821 = !{i64 5368716882}
!1948 = !{i64 5368717463}
!1963 = !{i64 5368717541}
!1990 = !{i64 5368717693}
!1889 = !{i64 5368717186}
!1814 = !{i64 5368716852}
!1897 = !{i64 5368717220}
!1956 = !{i64 5368717499}
!2018 = !{i64 5368717829}
!2019 = !{i64 5368717834}
!1817 = !{i64 5368716865}
!1837 = !{i64 5368716956}
!1924 = !{i64 5368717352}
!1991 = !{i64 5368717697}
!2022 = !{i64 5368717849}
!1966 = !{i64 5368717560}
!1805 = !{i64 5368716801}
!1947 = !{i64 5368717457}
!1982 = !{i64 5368717652}
!1967 = !{i64 5368717565}
!1806 = !{i64 5368716809}
!2020 = !{i64 5368717839}
!1900 = !{i64 5368717233}
!1953 = !{i64 5368717490}
!1992 = !{i64 5368717705}
!1832 = !{i64 5368716933}
!2023 = !{i64 5368717854}
!1932 = !{i64 5368717391}
!2035 = !{i64 5368717905}
!1865 = !{i64 5368717074}
!1996 = !{i64 5368717726}
!1969 = !{i64 5368717576}
!1850 = !{i64 5368717009}
!1927 = !{i64 5368717368}
!1884 = !{i64 5368717163}
!1894 = !{i64 5368717208}
!1803 = !{i64 5368716791}
!1901 = !{i64 5368717237}
!2029 = !{i64 5368717881}
!1885 = !{i64 5368717169}
!1985 = !{i64 5368717667}
!2003 = !{i64 5368717762}
!1973 = !{i64 5368717604}
!1906 = !{i64 5368717259}
!1987 = !{i64 5368717679}
!1886 = !{i64 5368717174}
!1949 = !{i64 5368717471}
!1823 = !{i64 5368716892}
!1864 = !{i64 5368717070}
!1846 = !{i64 5368716994}
!2000 = !{i64 5368717748}
!1842 = !{i64 5368716978}
!1856 = !{i64 5368717035}
!1955 = !{i64 5368717497}
!1801 = !{i64 5368717912}
!1828 = !{i64 5368716915}
!1870 = !{i64 5368717102}
!1830 = !{i64 5368716924}
!1859 = !{i64 5368717048}
!1879 = !{i64 5368717143}
!1972 = !{i64 5368717598}
!2008 = !{i64 5368717782}
!1974 = !{i64 5368717609}
!1995 = !{i64 5368717722}
!1890 = !{i64 5368717191}
!1907 = !{i64 5368717263}
!1962 = !{i64 5368717533}
!1971 = !{i64 5368717592}
!1867 = !{i64 5368717085}
!1914 = !{i64 5368717297}
!2033 = !{i64 5368717896}
!1820 = !{i64 5368716878}
!1888 = !{i64 5368717182}
!1874 = !{i64 5368717120}
!1923 = !{i64 5368717347}
!1937 = !{i64 5368717415}
!1808 = !{i64 5368716822}
!1807 = !{i64 5368716817}
!1847 = !{i64 5368716998}
!1943 = !{i64 5368717440}
!1849 = !{i64 5368717006}
!1898 = !{i64 5368717225}
!12 = !{i64 5369172168}
!11 = !{i64 5369172160}
!13 = !{i64 5369172176}
!15 = !{i64 5369172215}
!14 = !{i64 5369253888}
!4 = !{i64 5369253952}


declare i32 @sub_140001CB0(i64 %0, i64 %1)
declare i32 @sub_1400015F0(i32 %0)
declare i32 @sub_140001500(i32 %0)
declare i32 @sub_140001D70(i32 %0, i32 %1)
declare i32 @sub_1400013C0(i32 %0)
declare i32 @sub_140001C60(i64 %0, i32 %1)
declare i32 @sub_1400016A0(i32 %0)
declare i32 @sub_1400017B0(i32 %0)
declare float @sub_140001AE0(i64 %0, i32 %1, i32 %2)
declare i32 @sub_140001070(i32 %0, i32 %1)
declare i32 @sub_140001170(i32 %0)
declare i32 @sub_1400014E0(i32 %0, i32 %1)
declare i32 @sub_1400011F0(i32 %0)
declare i32 @sub_140001850(i32 %0, i32 %1)
declare i32 @sub_140001000(i32 %0, i32 %1)
declare i32 @sub_140001830(i32 %0)
declare i32 @sub_140001D10(i64 %0, i32 %1)
declare i32 @sub_140001110(i32 %0, i32 %1, i32 %2)
declare i32 @sub_140001240(i32 %0)
declare i32 @sub_1400017D0(i32 %0)
declare i32 @sub_1400012A0(i32 %0)
declare i64 @sub_1400019D0(i32 %0)
declare float @sub_140001A80(i64 %0, i64 %1, i32 %2)
declare i32 @sub_1400017F0(i32 %0)
declare float @sub_140001B30(i64 %0, i64 %1, i32 %2, i32 %3)
declare i32 @sub_140001810(i32 %0)
declare i32 @sub_140001470(i32 %0)
declare i32 @sub_140002400(i64 %0, i64 %1, i64 %2, i64 %3)
declare i32 @sub_1400010C0(i32 %0, i32 %1)
declare i32 @sub_140001BF0(i64 %0, i64 %1, i32 %2)
declare i32 @sub_1400016F0(i32 %0, i32 %1, i32 %2)
declare i32 @sub_140001580(i64 %0)
declare float @sub_140001A30(i64 %0, i32 %1)
declare i8 @llvm.ctpop.i8(i8) #0
declare i32 @sub_140001950(i64 %0, i64 %1, i32 %2)
declare void @sub_140002640(i64 %0)
declare i32 @sub_1400018E0(i64 %0, i8 %1, i32 %2)
declare i32 @sub_140001DB0(i32 %0, i32 %1)
declare double @sub_140001B90(i64 %0, i32 %1)

define i32 @main(i32 %0, i64 %1) #48 {
bb_140001df0:
  %frame = alloca [680 x i8], align 1, !revenant.va !1801
  %pf = alloca i1, align 1, !revenant.va !1801
  %of = alloca i1, align 1, !revenant.va !1801
  %cf = alloca i1, align 1, !revenant.va !1801
  %sf = alloca i1, align 1, !revenant.va !1801
  %zf = alloca i1, align 1, !revenant.va !1801
  %xmm15 = alloca i128, align 16, !revenant.va !1801
  %xmm14 = alloca i128, align 16, !revenant.va !1801
  %xmm13 = alloca i128, align 16, !revenant.va !1801
  %xmm12 = alloca i128, align 16, !revenant.va !1801
  %xmm11 = alloca i128, align 16, !revenant.va !1801
  %xmm10 = alloca i128, align 16, !revenant.va !1801
  %xmm9 = alloca i128, align 16, !revenant.va !1801
  %xmm8 = alloca i128, align 16, !revenant.va !1801
  %xmm7 = alloca i128, align 16, !revenant.va !1801
  %xmm6 = alloca i128, align 16, !revenant.va !1801
  %xmm5 = alloca i128, align 16, !revenant.va !1801
  %xmm4 = alloca i128, align 16, !revenant.va !1801
  %xmm3 = alloca i128, align 16, !revenant.va !1801
  %xmm2 = alloca i128, align 16, !revenant.va !1801
  %xmm1 = alloca i128, align 16, !revenant.va !1801
  %xmm0 = alloca i128, align 16, !revenant.va !1801
  %r15 = alloca i64, align 8, !revenant.va !1801
  %r14 = alloca i64, align 8, !revenant.va !1801
  %r13 = alloca i64, align 8, !revenant.va !1801
  %r12 = alloca i64, align 8, !revenant.va !1801
  %r11 = alloca i64, align 8, !revenant.va !1801
  %r10 = alloca i64, align 8, !revenant.va !1801
  %r9 = alloca i64, align 8, !revenant.va !1801
  %r8 = alloca i64, align 8, !revenant.va !1801
  %rbp = alloca i64, align 8, !revenant.va !1801
  %rdi = alloca i64, align 8, !revenant.va !1801
  %rsi = alloca i64, align 8, !revenant.va !1801
  %rbx = alloca i64, align 8, !revenant.va !1801
  %rdx = alloca i64, align 8, !revenant.va !1801
  %rcx = alloca i64, align 8, !revenant.va !1801
  %rax = alloca i64, align 8, !revenant.va !1801
  %sp0 = getelementptr inbounds i8, ptr %frame, i64 384, !revenant.va !1801
  %e = zext i32 %0 to i64, !revenant.va !1801
  store i64 %e, ptr %rcx, align 8, !revenant.va !1801
  store i64 %1, ptr %rdx, align 8, !revenant.va !1801
  store i64 undef, ptr %rax, align 8, !revenant.va !1801
  store i64 undef, ptr %rbx, align 8, !revenant.va !1801
  store i64 undef, ptr %rsi, align 8, !revenant.va !1801
  store i64 undef, ptr %rdi, align 8, !revenant.va !1801
  store i64 undef, ptr %rbp, align 8, !revenant.va !1801
  store i64 undef, ptr %r8, align 8, !revenant.va !1801
  store i64 undef, ptr %r9, align 8, !revenant.va !1801
  store i64 undef, ptr %r10, align 8, !revenant.va !1801
  store i64 undef, ptr %r11, align 8, !revenant.va !1801
  store i64 undef, ptr %r12, align 8, !revenant.va !1801
  store i64 undef, ptr %r13, align 8, !revenant.va !1801
  store i64 undef, ptr %r14, align 8, !revenant.va !1801
  store i64 undef, ptr %r15, align 8, !revenant.va !1801
  store i1 undef, ptr %cf, align 1, !revenant.va !1801
  store i1 undef, ptr %of, align 1, !revenant.va !1801
  store i1 undef, ptr %pf, align 1, !revenant.va !1801
  store i1 undef, ptr %sf, align 1, !revenant.va !1801
  store i1 undef, ptr %zf, align 1, !revenant.va !1801
  %fp = getelementptr inbounds i8, ptr %sp0, i64 0, !revenant.va !1802
  %rsp = ptrtoint ptr %fp to i64, !revenant.va !1802
  %2 = sub i64 %rsp, 376, !revenant.va !1802
  %zf1 = icmp eq i64 %2, 0, !revenant.va !1802
  store i1 %zf1, ptr %zf, align 1, !revenant.va !1802
  %sf2 = and i64 %2, -9223372036854775808, !revenant.va !1802
  %sf3 = icmp ne i64 %sf2, 0, !revenant.va !1802
  store i1 %sf3, ptr %sf, align 1, !revenant.va !1802
  %l = trunc i64 %2 to i8, !revenant.va !1802
  %3 = call i8 @llvm.ctpop.i8(i8 %l), !revenant.va !1802
  %pf4 = trunc i8 %3 to i1, !revenant.va !1802
  %pf5 = xor i1 %pf4, true, !revenant.va !1802
  store i1 %pf5, ptr %pf, align 1, !revenant.va !1802
  %cf6 = icmp ult i64 %rsp, 376, !revenant.va !1802
  store i1 %cf6, ptr %cf, align 1, !revenant.va !1802
  %o = xor i64 %rsp, 376, !revenant.va !1802
  %o2 = xor i64 %rsp, %2, !revenant.va !1802
  %o3 = and i64 %o, %o2, !revenant.va !1802
  %o4 = and i64 %o3, -9223372036854775808, !revenant.va !1802
  %of7 = icmp ne i64 %o4, 0, !revenant.va !1802
  store i1 %of7, ptr %of, align 1, !revenant.va !1802
  %4 = load i64, ptr @g_140085040.2, align 1, !revenant.va !1803
  store i64 %4, ptr %rax, align 8, !revenant.va !1803
  %fp8 = getelementptr inbounds i8, ptr %sp0, i64 -376, !revenant.va !1804
  %rsp9 = ptrtoint ptr %fp8 to i64, !revenant.va !1804
  %r = load i64, ptr %rax, align 8, !revenant.va !1804
  %5 = xor i64 %r, %rsp9, !revenant.va !1804
  %zf10 = icmp eq i64 %5, 0, !revenant.va !1804
  store i1 %zf10, ptr %zf, align 1, !revenant.va !1804
  %sf11 = and i64 %5, -9223372036854775808, !revenant.va !1804
  %sf12 = icmp ne i64 %sf11, 0, !revenant.va !1804
  store i1 %sf12, ptr %sf, align 1, !revenant.va !1804
  %l13 = trunc i64 %5 to i8, !revenant.va !1804
  %6 = call i8 @llvm.ctpop.i8(i8 %l13), !revenant.va !1804
  %pf14 = trunc i8 %6 to i1, !revenant.va !1804
  %pf15 = xor i1 %pf14, true, !revenant.va !1804
  store i1 %pf15, ptr %pf, align 1, !revenant.va !1804
  store i1 false, ptr %cf, align 1, !revenant.va !1804
  store i1 false, ptr %of, align 1, !revenant.va !1804
  store i64 %5, ptr %rax, align 8, !revenant.va !1804
  %r16 = load i64, ptr %rax, align 8, !revenant.va !1805
  %fp17 = getelementptr inbounds i8, ptr %sp0, i64 -8, !revenant.va !1805
  %7 = ptrtoint ptr %fp17 to i64, !revenant.va !1805
  store i64 %r16, ptr %fp17, align 1, !revenant.va !1805
  %fp18 = getelementptr inbounds i8, ptr %sp0, i64 -300, !revenant.va !1806
  %8 = ptrtoint ptr %fp18 to i64, !revenant.va !1806
  store i32 0, ptr %fp18, align 1, !revenant.va !1806
  %r19 = load i64, ptr %rdx, align 8, !revenant.va !1807
  %fp20 = getelementptr inbounds i8, ptr %sp0, i64 -312, !revenant.va !1807
  %9 = ptrtoint ptr %fp20 to i64, !revenant.va !1807
  store i64 %r19, ptr %fp20, align 1, !revenant.va !1807
  %r21 = load i64, ptr %rcx, align 8, !revenant.va !1808
  %t = trunc i64 %r21 to i32, !revenant.va !1808
  %fp22 = getelementptr inbounds i8, ptr %sp0, i64 -316, !revenant.va !1808
  %10 = ptrtoint ptr %fp22 to i64, !revenant.va !1808
  store i32 %t, ptr %fp22, align 1, !revenant.va !1808
  %fp23 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1809
  %11 = ptrtoint ptr %fp23 to i64, !revenant.va !1809
  store i32 0, ptr %fp23, align 1, !revenant.va !1809
  %fp24 = getelementptr inbounds i8, ptr %sp0, i64 -316, !revenant.va !1810
  %12 = ptrtoint ptr %fp24 to i64, !revenant.va !1810
  %13 = load i32, ptr %fp24, align 1, !revenant.va !1810
  %z = zext i32 %13 to i64, !revenant.va !1810
  store i64 %z, ptr %rdx, align 8, !revenant.va !1810
  store i64 10, ptr %rcx, align 8, !revenant.va !1811
  %r25 = load i64, ptr %rcx, align 8, !revenant.va !1812
  %e26 = trunc i64 %r25 to i32, !revenant.va !1812
  %r27 = load i64, ptr %rdx, align 8, !revenant.va !1812
  %e28 = trunc i64 %r27 to i32, !revenant.va !1812
  %c = call i32 @sub_140001000(i32 %e26, i32 %e28), !revenant.va !1812
  store i64 undef, ptr %rax, align 8, !revenant.va !1812
  store i64 undef, ptr %rcx, align 8, !revenant.va !1812
  store i64 undef, ptr %rdx, align 8, !revenant.va !1812
  store i1 undef, ptr %cf, align 1, !revenant.va !1812
  store i1 undef, ptr %of, align 1, !revenant.va !1812
  store i1 undef, ptr %pf, align 1, !revenant.va !1812
  store i1 undef, ptr %sf, align 1, !revenant.va !1812
  store i1 undef, ptr %zf, align 1, !revenant.va !1812
  %e29 = zext i32 %c to i64, !revenant.va !1812
  store i64 %e29, ptr %rax, align 8, !revenant.va !1812
  %fp30 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1813
  %14 = ptrtoint ptr %fp30 to i64, !revenant.va !1813
  %15 = load i32, ptr %fp30, align 1, !revenant.va !1813
  %r31 = load i64, ptr %rax, align 8, !revenant.va !1813
  %t32 = trunc i64 %r31 to i32, !revenant.va !1813
  %16 = add i32 %t32, %15, !revenant.va !1813
  %zf33 = icmp eq i32 %16, 0, !revenant.va !1813
  store i1 %zf33, ptr %zf, align 1, !revenant.va !1813
  %sf34 = and i32 %16, -2147483648, !revenant.va !1813
  %sf35 = icmp ne i32 %sf34, 0, !revenant.va !1813
  store i1 %sf35, ptr %sf, align 1, !revenant.va !1813
  %l36 = trunc i32 %16 to i8, !revenant.va !1813
  %17 = call i8 @llvm.ctpop.i8(i8 %l36), !revenant.va !1813
  %pf37 = trunc i8 %17 to i1, !revenant.va !1813
  %pf38 = xor i1 %pf37, true, !revenant.va !1813
  store i1 %pf38, ptr %pf, align 1, !revenant.va !1813
  %cf39 = icmp ult i32 %16, %t32, !revenant.va !1813
  store i1 %cf39, ptr %cf, align 1, !revenant.va !1813
  %o40 = xor i32 %t32, %15, !revenant.va !1813
  %n = xor i32 %o40, -1, !revenant.va !1813
  %o241 = xor i32 %t32, %16, !revenant.va !1813
  %o342 = and i32 %n, %o241, !revenant.va !1813
  %o443 = and i32 %o342, -2147483648, !revenant.va !1813
  %of44 = icmp ne i32 %o443, 0, !revenant.va !1813
  store i1 %of44, ptr %of, align 1, !revenant.va !1813
  %z45 = zext i32 %16 to i64, !revenant.va !1813
  store i64 %z45, ptr %rax, align 8, !revenant.va !1813
  %r46 = load i64, ptr %rax, align 8, !revenant.va !1814
  %t47 = trunc i64 %r46 to i32, !revenant.va !1814
  %fp48 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1814
  %18 = ptrtoint ptr %fp48 to i64, !revenant.va !1814
  store i32 %t47, ptr %fp48, align 1, !revenant.va !1814
  %fp49 = getelementptr inbounds i8, ptr %sp0, i64 -316, !revenant.va !1815
  %19 = ptrtoint ptr %fp49 to i64, !revenant.va !1815
  %20 = load i32, ptr %fp49, align 1, !revenant.va !1815
  %z50 = zext i32 %20 to i64, !revenant.va !1815
  store i64 %z50, ptr %rdx, align 8, !revenant.va !1815
  store i64 3, ptr %rcx, align 8, !revenant.va !1816
  %r51 = load i64, ptr %rcx, align 8, !revenant.va !1817
  %e52 = trunc i64 %r51 to i32, !revenant.va !1817
  %r53 = load i64, ptr %rdx, align 8, !revenant.va !1817
  %e54 = trunc i64 %r53 to i32, !revenant.va !1817
  %c55 = call i32 @sub_140001070(i32 %e52, i32 %e54), !revenant.va !1817
  store i64 undef, ptr %rax, align 8, !revenant.va !1817
  store i1 undef, ptr %cf, align 1, !revenant.va !1817
  store i1 undef, ptr %of, align 1, !revenant.va !1817
  store i1 undef, ptr %pf, align 1, !revenant.va !1817
  store i1 undef, ptr %sf, align 1, !revenant.va !1817
  store i1 undef, ptr %zf, align 1, !revenant.va !1817
  %e56 = zext i32 %c55 to i64, !revenant.va !1817
  store i64 %e56, ptr %rax, align 8, !revenant.va !1817
  %fp57 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1818
  %21 = ptrtoint ptr %fp57 to i64, !revenant.va !1818
  %22 = load i32, ptr %fp57, align 1, !revenant.va !1818
  %r58 = load i64, ptr %rax, align 8, !revenant.va !1818
  %t59 = trunc i64 %r58 to i32, !revenant.va !1818
  %23 = add i32 %t59, %22, !revenant.va !1818
  %zf60 = icmp eq i32 %23, 0, !revenant.va !1818
  store i1 %zf60, ptr %zf, align 1, !revenant.va !1818
  %sf61 = and i32 %23, -2147483648, !revenant.va !1818
  %sf62 = icmp ne i32 %sf61, 0, !revenant.va !1818
  store i1 %sf62, ptr %sf, align 1, !revenant.va !1818
  %l63 = trunc i32 %23 to i8, !revenant.va !1818
  %24 = call i8 @llvm.ctpop.i8(i8 %l63), !revenant.va !1818
  %pf64 = trunc i8 %24 to i1, !revenant.va !1818
  %pf65 = xor i1 %pf64, true, !revenant.va !1818
  store i1 %pf65, ptr %pf, align 1, !revenant.va !1818
  %cf66 = icmp ult i32 %23, %t59, !revenant.va !1818
  store i1 %cf66, ptr %cf, align 1, !revenant.va !1818
  %o67 = xor i32 %t59, %22, !revenant.va !1818
  %n68 = xor i32 %o67, -1, !revenant.va !1818
  %o269 = xor i32 %t59, %23, !revenant.va !1818
  %o370 = and i32 %n68, %o269, !revenant.va !1818
  %o471 = and i32 %o370, -2147483648, !revenant.va !1818
  %of72 = icmp ne i32 %o471, 0, !revenant.va !1818
  store i1 %of72, ptr %of, align 1, !revenant.va !1818
  %z73 = zext i32 %23 to i64, !revenant.va !1818
  store i64 %z73, ptr %rax, align 8, !revenant.va !1818
  %r74 = load i64, ptr %rax, align 8, !revenant.va !1819
  %t75 = trunc i64 %r74 to i32, !revenant.va !1819
  %fp76 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1819
  %25 = ptrtoint ptr %fp76 to i64, !revenant.va !1819
  store i32 %t75, ptr %fp76, align 1, !revenant.va !1819
  %fp77 = getelementptr inbounds i8, ptr %sp0, i64 -316, !revenant.va !1820
  %26 = ptrtoint ptr %fp77 to i64, !revenant.va !1820
  %27 = load i32, ptr %fp77, align 1, !revenant.va !1820
  %z78 = zext i32 %27 to i64, !revenant.va !1820
  store i64 %z78, ptr %rdx, align 8, !revenant.va !1820
  store i64 3, ptr %rcx, align 8, !revenant.va !1821
  %r79 = load i64, ptr %rcx, align 8, !revenant.va !1822
  %e80 = trunc i64 %r79 to i32, !revenant.va !1822
  %r81 = load i64, ptr %rdx, align 8, !revenant.va !1822
  %e82 = trunc i64 %r81 to i32, !revenant.va !1822
  %c83 = call i32 @sub_1400010C0(i32 %e80, i32 %e82), !revenant.va !1822
  store i64 undef, ptr %rax, align 8, !revenant.va !1822
  store i1 undef, ptr %cf, align 1, !revenant.va !1822
  store i1 undef, ptr %of, align 1, !revenant.va !1822
  store i1 undef, ptr %pf, align 1, !revenant.va !1822
  store i1 undef, ptr %sf, align 1, !revenant.va !1822
  store i1 undef, ptr %zf, align 1, !revenant.va !1822
  %e84 = zext i32 %c83 to i64, !revenant.va !1822
  store i64 %e84, ptr %rax, align 8, !revenant.va !1822
  %fp85 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1823
  %28 = ptrtoint ptr %fp85 to i64, !revenant.va !1823
  %29 = load i32, ptr %fp85, align 1, !revenant.va !1823
  %r86 = load i64, ptr %rax, align 8, !revenant.va !1823
  %t87 = trunc i64 %r86 to i32, !revenant.va !1823
  %30 = add i32 %t87, %29, !revenant.va !1823
  %zf88 = icmp eq i32 %30, 0, !revenant.va !1823
  store i1 %zf88, ptr %zf, align 1, !revenant.va !1823
  %sf89 = and i32 %30, -2147483648, !revenant.va !1823
  %sf90 = icmp ne i32 %sf89, 0, !revenant.va !1823
  store i1 %sf90, ptr %sf, align 1, !revenant.va !1823
  %l91 = trunc i32 %30 to i8, !revenant.va !1823
  %31 = call i8 @llvm.ctpop.i8(i8 %l91), !revenant.va !1823
  %pf92 = trunc i8 %31 to i1, !revenant.va !1823
  %pf93 = xor i1 %pf92, true, !revenant.va !1823
  store i1 %pf93, ptr %pf, align 1, !revenant.va !1823
  %cf94 = icmp ult i32 %30, %t87, !revenant.va !1823
  store i1 %cf94, ptr %cf, align 1, !revenant.va !1823
  %o95 = xor i32 %t87, %29, !revenant.va !1823
  %n96 = xor i32 %o95, -1, !revenant.va !1823
  %o297 = xor i32 %t87, %30, !revenant.va !1823
  %o398 = and i32 %n96, %o297, !revenant.va !1823
  %o499 = and i32 %o398, -2147483648, !revenant.va !1823
  %of100 = icmp ne i32 %o499, 0, !revenant.va !1823
  store i1 %of100, ptr %of, align 1, !revenant.va !1823
  %z101 = zext i32 %30 to i64, !revenant.va !1823
  store i64 %z101, ptr %rax, align 8, !revenant.va !1823
  %r102 = load i64, ptr %rax, align 8, !revenant.va !1824
  %t103 = trunc i64 %r102 to i32, !revenant.va !1824
  %fp104 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1824
  %32 = ptrtoint ptr %fp104 to i64, !revenant.va !1824
  store i32 %t103, ptr %fp104, align 1, !revenant.va !1824
  %fp105 = getelementptr inbounds i8, ptr %sp0, i64 -316, !revenant.va !1825
  %33 = ptrtoint ptr %fp105 to i64, !revenant.va !1825
  %34 = load i32, ptr %fp105, align 1, !revenant.va !1825
  %z106 = zext i32 %34 to i64, !revenant.va !1825
  store i64 %z106, ptr %r8, align 8, !revenant.va !1825
  store i64 1, ptr %rcx, align 8, !revenant.va !1826
  store i64 2, ptr %rdx, align 8, !revenant.va !1827
  %r107 = load i64, ptr %rcx, align 8, !revenant.va !1828
  %e108 = trunc i64 %r107 to i32, !revenant.va !1828
  %r109 = load i64, ptr %rdx, align 8, !revenant.va !1828
  %e110 = trunc i64 %r109 to i32, !revenant.va !1828
  %r111 = load i64, ptr %r8, align 8, !revenant.va !1828
  %e112 = trunc i64 %r111 to i32, !revenant.va !1828
  %c113 = call i32 @sub_140001110(i32 %e108, i32 %e110, i32 %e112), !revenant.va !1828
  store i64 undef, ptr %rax, align 8, !revenant.va !1828
  store i1 undef, ptr %cf, align 1, !revenant.va !1828
  store i1 undef, ptr %of, align 1, !revenant.va !1828
  store i1 undef, ptr %pf, align 1, !revenant.va !1828
  store i1 undef, ptr %sf, align 1, !revenant.va !1828
  store i1 undef, ptr %zf, align 1, !revenant.va !1828
  %e114 = zext i32 %c113 to i64, !revenant.va !1828
  store i64 %e114, ptr %rax, align 8, !revenant.va !1828
  %fp115 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1829
  %35 = ptrtoint ptr %fp115 to i64, !revenant.va !1829
  %36 = load i32, ptr %fp115, align 1, !revenant.va !1829
  %r116 = load i64, ptr %rax, align 8, !revenant.va !1829
  %t117 = trunc i64 %r116 to i32, !revenant.va !1829
  %37 = add i32 %t117, %36, !revenant.va !1829
  %zf118 = icmp eq i32 %37, 0, !revenant.va !1829
  store i1 %zf118, ptr %zf, align 1, !revenant.va !1829
  %sf119 = and i32 %37, -2147483648, !revenant.va !1829
  %sf120 = icmp ne i32 %sf119, 0, !revenant.va !1829
  store i1 %sf120, ptr %sf, align 1, !revenant.va !1829
  %l121 = trunc i32 %37 to i8, !revenant.va !1829
  %38 = call i8 @llvm.ctpop.i8(i8 %l121), !revenant.va !1829
  %pf122 = trunc i8 %38 to i1, !revenant.va !1829
  %pf123 = xor i1 %pf122, true, !revenant.va !1829
  store i1 %pf123, ptr %pf, align 1, !revenant.va !1829
  %cf124 = icmp ult i32 %37, %t117, !revenant.va !1829
  store i1 %cf124, ptr %cf, align 1, !revenant.va !1829
  %o125 = xor i32 %t117, %36, !revenant.va !1829
  %n126 = xor i32 %o125, -1, !revenant.va !1829
  %o2127 = xor i32 %t117, %37, !revenant.va !1829
  %o3128 = and i32 %n126, %o2127, !revenant.va !1829
  %o4129 = and i32 %o3128, -2147483648, !revenant.va !1829
  %of130 = icmp ne i32 %o4129, 0, !revenant.va !1829
  store i1 %of130, ptr %of, align 1, !revenant.va !1829
  %z131 = zext i32 %37 to i64, !revenant.va !1829
  store i64 %z131, ptr %rax, align 8, !revenant.va !1829
  %r132 = load i64, ptr %rax, align 8, !revenant.va !1830
  %t133 = trunc i64 %r132 to i32, !revenant.va !1830
  %fp134 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1830
  %39 = ptrtoint ptr %fp134 to i64, !revenant.va !1830
  store i32 %t133, ptr %fp134, align 1, !revenant.va !1830
  store i64 25, ptr %rcx, align 8, !revenant.va !1831
  %r135 = load i64, ptr %rcx, align 8, !revenant.va !1832
  %e136 = trunc i64 %r135 to i32, !revenant.va !1832
  %c137 = call i32 @sub_140001170(i32 %e136), !revenant.va !1832
  store i64 undef, ptr %rax, align 8, !revenant.va !1832
  store i1 undef, ptr %cf, align 1, !revenant.va !1832
  store i1 undef, ptr %of, align 1, !revenant.va !1832
  store i1 undef, ptr %pf, align 1, !revenant.va !1832
  store i1 undef, ptr %sf, align 1, !revenant.va !1832
  store i1 undef, ptr %zf, align 1, !revenant.va !1832
  %e138 = zext i32 %c137 to i64, !revenant.va !1832
  store i64 %e138, ptr %rax, align 8, !revenant.va !1832
  %fp139 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1833
  %40 = ptrtoint ptr %fp139 to i64, !revenant.va !1833
  %41 = load i32, ptr %fp139, align 1, !revenant.va !1833
  %r140 = load i64, ptr %rax, align 8, !revenant.va !1833
  %t141 = trunc i64 %r140 to i32, !revenant.va !1833
  %42 = add i32 %t141, %41, !revenant.va !1833
  %zf142 = icmp eq i32 %42, 0, !revenant.va !1833
  store i1 %zf142, ptr %zf, align 1, !revenant.va !1833
  %sf143 = and i32 %42, -2147483648, !revenant.va !1833
  %sf144 = icmp ne i32 %sf143, 0, !revenant.va !1833
  store i1 %sf144, ptr %sf, align 1, !revenant.va !1833
  %l145 = trunc i32 %42 to i8, !revenant.va !1833
  %43 = call i8 @llvm.ctpop.i8(i8 %l145), !revenant.va !1833
  %pf146 = trunc i8 %43 to i1, !revenant.va !1833
  %pf147 = xor i1 %pf146, true, !revenant.va !1833
  store i1 %pf147, ptr %pf, align 1, !revenant.va !1833
  %cf148 = icmp ult i32 %42, %t141, !revenant.va !1833
  store i1 %cf148, ptr %cf, align 1, !revenant.va !1833
  %o149 = xor i32 %t141, %41, !revenant.va !1833
  %n150 = xor i32 %o149, -1, !revenant.va !1833
  %o2151 = xor i32 %t141, %42, !revenant.va !1833
  %o3152 = and i32 %n150, %o2151, !revenant.va !1833
  %o4153 = and i32 %o3152, -2147483648, !revenant.va !1833
  %of154 = icmp ne i32 %o4153, 0, !revenant.va !1833
  store i1 %of154, ptr %of, align 1, !revenant.va !1833
  %z155 = zext i32 %42 to i64, !revenant.va !1833
  store i64 %z155, ptr %rax, align 8, !revenant.va !1833
  %r156 = load i64, ptr %rax, align 8, !revenant.va !1834
  %t157 = trunc i64 %r156 to i32, !revenant.va !1834
  %fp158 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1834
  %44 = ptrtoint ptr %fp158 to i64, !revenant.va !1834
  store i32 %t157, ptr %fp158, align 1, !revenant.va !1834
  store i64 5, ptr %rcx, align 8, !revenant.va !1835
  %r159 = load i64, ptr %rcx, align 8, !revenant.va !1836
  %e160 = trunc i64 %r159 to i32, !revenant.va !1836
  %c161 = call i32 @sub_1400011F0(i32 %e160), !revenant.va !1836
  store i64 undef, ptr %rax, align 8, !revenant.va !1836
  store i64 undef, ptr %rcx, align 8, !revenant.va !1836
  store i1 undef, ptr %cf, align 1, !revenant.va !1836
  store i1 undef, ptr %of, align 1, !revenant.va !1836
  store i1 undef, ptr %pf, align 1, !revenant.va !1836
  store i1 undef, ptr %sf, align 1, !revenant.va !1836
  store i1 undef, ptr %zf, align 1, !revenant.va !1836
  %e162 = zext i32 %c161 to i64, !revenant.va !1836
  store i64 %e162, ptr %rax, align 8, !revenant.va !1836
  %fp163 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1837
  %45 = ptrtoint ptr %fp163 to i64, !revenant.va !1837
  %46 = load i32, ptr %fp163, align 1, !revenant.va !1837
  %r164 = load i64, ptr %rax, align 8, !revenant.va !1837
  %t165 = trunc i64 %r164 to i32, !revenant.va !1837
  %47 = add i32 %t165, %46, !revenant.va !1837
  %zf166 = icmp eq i32 %47, 0, !revenant.va !1837
  store i1 %zf166, ptr %zf, align 1, !revenant.va !1837
  %sf167 = and i32 %47, -2147483648, !revenant.va !1837
  %sf168 = icmp ne i32 %sf167, 0, !revenant.va !1837
  store i1 %sf168, ptr %sf, align 1, !revenant.va !1837
  %l169 = trunc i32 %47 to i8, !revenant.va !1837
  %48 = call i8 @llvm.ctpop.i8(i8 %l169), !revenant.va !1837
  %pf170 = trunc i8 %48 to i1, !revenant.va !1837
  %pf171 = xor i1 %pf170, true, !revenant.va !1837
  store i1 %pf171, ptr %pf, align 1, !revenant.va !1837
  %cf172 = icmp ult i32 %47, %t165, !revenant.va !1837
  store i1 %cf172, ptr %cf, align 1, !revenant.va !1837
  %o173 = xor i32 %t165, %46, !revenant.va !1837
  %n174 = xor i32 %o173, -1, !revenant.va !1837
  %o2175 = xor i32 %t165, %47, !revenant.va !1837
  %o3176 = and i32 %n174, %o2175, !revenant.va !1837
  %o4177 = and i32 %o3176, -2147483648, !revenant.va !1837
  %of178 = icmp ne i32 %o4177, 0, !revenant.va !1837
  store i1 %of178, ptr %of, align 1, !revenant.va !1837
  %z179 = zext i32 %47 to i64, !revenant.va !1837
  store i64 %z179, ptr %rax, align 8, !revenant.va !1837
  %r180 = load i64, ptr %rax, align 8, !revenant.va !1838
  %t181 = trunc i64 %r180 to i32, !revenant.va !1838
  %fp182 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1838
  %49 = ptrtoint ptr %fp182 to i64, !revenant.va !1838
  store i32 %t181, ptr %fp182, align 1, !revenant.va !1838
  store i64 30, ptr %rcx, align 8, !revenant.va !1839
  %r183 = load i64, ptr %rcx, align 8, !revenant.va !1840
  %e184 = trunc i64 %r183 to i32, !revenant.va !1840
  %c185 = call i32 @sub_140001240(i32 %e184), !revenant.va !1840
  store i64 undef, ptr %rax, align 8, !revenant.va !1840
  store i64 undef, ptr %rcx, align 8, !revenant.va !1840
  store i64 undef, ptr %rdx, align 8, !revenant.va !1840
  store i1 undef, ptr %cf, align 1, !revenant.va !1840
  store i1 undef, ptr %of, align 1, !revenant.va !1840
  store i1 undef, ptr %pf, align 1, !revenant.va !1840
  store i1 undef, ptr %sf, align 1, !revenant.va !1840
  store i1 undef, ptr %zf, align 1, !revenant.va !1840
  %e186 = zext i32 %c185 to i64, !revenant.va !1840
  store i64 %e186, ptr %rax, align 8, !revenant.va !1840
  %fp187 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1841
  %50 = ptrtoint ptr %fp187 to i64, !revenant.va !1841
  %51 = load i32, ptr %fp187, align 1, !revenant.va !1841
  %r188 = load i64, ptr %rax, align 8, !revenant.va !1841
  %t189 = trunc i64 %r188 to i32, !revenant.va !1841
  %52 = add i32 %t189, %51, !revenant.va !1841
  %zf190 = icmp eq i32 %52, 0, !revenant.va !1841
  store i1 %zf190, ptr %zf, align 1, !revenant.va !1841
  %sf191 = and i32 %52, -2147483648, !revenant.va !1841
  %sf192 = icmp ne i32 %sf191, 0, !revenant.va !1841
  store i1 %sf192, ptr %sf, align 1, !revenant.va !1841
  %l193 = trunc i32 %52 to i8, !revenant.va !1841
  %53 = call i8 @llvm.ctpop.i8(i8 %l193), !revenant.va !1841
  %pf194 = trunc i8 %53 to i1, !revenant.va !1841
  %pf195 = xor i1 %pf194, true, !revenant.va !1841
  store i1 %pf195, ptr %pf, align 1, !revenant.va !1841
  %cf196 = icmp ult i32 %52, %t189, !revenant.va !1841
  store i1 %cf196, ptr %cf, align 1, !revenant.va !1841
  %o197 = xor i32 %t189, %51, !revenant.va !1841
  %n198 = xor i32 %o197, -1, !revenant.va !1841
  %o2199 = xor i32 %t189, %52, !revenant.va !1841
  %o3200 = and i32 %n198, %o2199, !revenant.va !1841
  %o4201 = and i32 %o3200, -2147483648, !revenant.va !1841
  %of202 = icmp ne i32 %o4201, 0, !revenant.va !1841
  store i1 %of202, ptr %of, align 1, !revenant.va !1841
  %z203 = zext i32 %52 to i64, !revenant.va !1841
  store i64 %z203, ptr %rax, align 8, !revenant.va !1841
  %r204 = load i64, ptr %rax, align 8, !revenant.va !1842
  %t205 = trunc i64 %r204 to i32, !revenant.va !1842
  %fp206 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1842
  %54 = ptrtoint ptr %fp206 to i64, !revenant.va !1842
  store i32 %t205, ptr %fp206, align 1, !revenant.va !1842
  %fp207 = getelementptr inbounds i8, ptr %sp0, i64 -316, !revenant.va !1843
  %55 = ptrtoint ptr %fp207 to i64, !revenant.va !1843
  %56 = load i32, ptr %fp207, align 1, !revenant.va !1843
  %z208 = zext i32 %56 to i64, !revenant.va !1843
  store i64 %z208, ptr %rcx, align 8, !revenant.va !1843
  %r209 = load i64, ptr %rcx, align 8, !revenant.va !1844
  %t210 = trunc i64 %r209 to i32, !revenant.va !1844
  %57 = add i32 %t210, 5, !revenant.va !1844
  %zf211 = icmp eq i32 %57, 0, !revenant.va !1844
  store i1 %zf211, ptr %zf, align 1, !revenant.va !1844
  %sf212 = and i32 %57, -2147483648, !revenant.va !1844
  %sf213 = icmp ne i32 %sf212, 0, !revenant.va !1844
  store i1 %sf213, ptr %sf, align 1, !revenant.va !1844
  %l214 = trunc i32 %57 to i8, !revenant.va !1844
  %58 = call i8 @llvm.ctpop.i8(i8 %l214), !revenant.va !1844
  %pf215 = trunc i8 %58 to i1, !revenant.va !1844
  %pf216 = xor i1 %pf215, true, !revenant.va !1844
  store i1 %pf216, ptr %pf, align 1, !revenant.va !1844
  %cf217 = icmp ult i32 %57, %t210, !revenant.va !1844
  store i1 %cf217, ptr %cf, align 1, !revenant.va !1844
  %o218 = xor i32 %t210, 5, !revenant.va !1844
  %n219 = xor i32 %o218, -1, !revenant.va !1844
  %o2220 = xor i32 %t210, %57, !revenant.va !1844
  %o3221 = and i32 %n219, %o2220, !revenant.va !1844
  %o4222 = and i32 %o3221, -2147483648, !revenant.va !1844
  %of223 = icmp ne i32 %o4222, 0, !revenant.va !1844
  store i1 %of223, ptr %of, align 1, !revenant.va !1844
  %z224 = zext i32 %57 to i64, !revenant.va !1844
  store i64 %z224, ptr %rcx, align 8, !revenant.va !1844
  %r225 = load i64, ptr %rcx, align 8, !revenant.va !1845
  %e226 = trunc i64 %r225 to i32, !revenant.va !1845
  %c227 = call i32 @sub_1400012A0(i32 %e226), !revenant.va !1845
  store i64 undef, ptr %rax, align 8, !revenant.va !1845
  store i64 undef, ptr %rcx, align 8, !revenant.va !1845
  store i64 undef, ptr %rdx, align 8, !revenant.va !1845
  store i64 undef, ptr %r8, align 8, !revenant.va !1845
  store i64 undef, ptr %r9, align 8, !revenant.va !1845
  store i64 undef, ptr %r10, align 8, !revenant.va !1845
  store i64 undef, ptr %r11, align 8, !revenant.va !1845
  store i128 undef, ptr %xmm0, align 16, !revenant.va !1845
  store i128 undef, ptr %xmm1, align 16, !revenant.va !1845
  store i128 undef, ptr %xmm2, align 16, !revenant.va !1845
  store i128 undef, ptr %xmm3, align 16, !revenant.va !1845
  store i128 undef, ptr %xmm4, align 16, !revenant.va !1845
  store i128 undef, ptr %xmm5, align 16, !revenant.va !1845
  store i1 undef, ptr %cf, align 1, !revenant.va !1845
  store i1 undef, ptr %of, align 1, !revenant.va !1845
  store i1 undef, ptr %pf, align 1, !revenant.va !1845
  store i1 undef, ptr %sf, align 1, !revenant.va !1845
  store i1 undef, ptr %zf, align 1, !revenant.va !1845
  %e228 = zext i32 %c227 to i64, !revenant.va !1845
  store i64 %e228, ptr %rax, align 8, !revenant.va !1845
  %fp229 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1846
  %59 = ptrtoint ptr %fp229 to i64, !revenant.va !1846
  %60 = load i32, ptr %fp229, align 1, !revenant.va !1846
  %r230 = load i64, ptr %rax, align 8, !revenant.va !1846
  %t231 = trunc i64 %r230 to i32, !revenant.va !1846
  %61 = add i32 %t231, %60, !revenant.va !1846
  %zf232 = icmp eq i32 %61, 0, !revenant.va !1846
  store i1 %zf232, ptr %zf, align 1, !revenant.va !1846
  %sf233 = and i32 %61, -2147483648, !revenant.va !1846
  %sf234 = icmp ne i32 %sf233, 0, !revenant.va !1846
  store i1 %sf234, ptr %sf, align 1, !revenant.va !1846
  %l235 = trunc i32 %61 to i8, !revenant.va !1846
  %62 = call i8 @llvm.ctpop.i8(i8 %l235), !revenant.va !1846
  %pf236 = trunc i8 %62 to i1, !revenant.va !1846
  %pf237 = xor i1 %pf236, true, !revenant.va !1846
  store i1 %pf237, ptr %pf, align 1, !revenant.va !1846
  %cf238 = icmp ult i32 %61, %t231, !revenant.va !1846
  store i1 %cf238, ptr %cf, align 1, !revenant.va !1846
  %o239 = xor i32 %t231, %60, !revenant.va !1846
  %n240 = xor i32 %o239, -1, !revenant.va !1846
  %o2241 = xor i32 %t231, %61, !revenant.va !1846
  %o3242 = and i32 %n240, %o2241, !revenant.va !1846
  %o4243 = and i32 %o3242, -2147483648, !revenant.va !1846
  %of244 = icmp ne i32 %o4243, 0, !revenant.va !1846
  store i1 %of244, ptr %of, align 1, !revenant.va !1846
  %z245 = zext i32 %61 to i64, !revenant.va !1846
  store i64 %z245, ptr %rax, align 8, !revenant.va !1846
  %r246 = load i64, ptr %rax, align 8, !revenant.va !1847
  %t247 = trunc i64 %r246 to i32, !revenant.va !1847
  %fp248 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1847
  %63 = ptrtoint ptr %fp248 to i64, !revenant.va !1847
  store i32 %t247, ptr %fp248, align 1, !revenant.va !1847
  %fp249 = getelementptr inbounds i8, ptr %sp0, i64 -316, !revenant.va !1848
  %64 = ptrtoint ptr %fp249 to i64, !revenant.va !1848
  %65 = load i32, ptr %fp249, align 1, !revenant.va !1848
  %z250 = zext i32 %65 to i64, !revenant.va !1848
  store i64 %z250, ptr %rcx, align 8, !revenant.va !1848
  %r251 = load i64, ptr %rcx, align 8, !revenant.va !1849
  %t252 = trunc i64 %r251 to i32, !revenant.va !1849
  %66 = add i32 %t252, 2, !revenant.va !1849
  %zf253 = icmp eq i32 %66, 0, !revenant.va !1849
  store i1 %zf253, ptr %zf, align 1, !revenant.va !1849
  %sf254 = and i32 %66, -2147483648, !revenant.va !1849
  %sf255 = icmp ne i32 %sf254, 0, !revenant.va !1849
  store i1 %sf255, ptr %sf, align 1, !revenant.va !1849
  %l256 = trunc i32 %66 to i8, !revenant.va !1849
  %67 = call i8 @llvm.ctpop.i8(i8 %l256), !revenant.va !1849
  %pf257 = trunc i8 %67 to i1, !revenant.va !1849
  %pf258 = xor i1 %pf257, true, !revenant.va !1849
  store i1 %pf258, ptr %pf, align 1, !revenant.va !1849
  %cf259 = icmp ult i32 %66, %t252, !revenant.va !1849
  store i1 %cf259, ptr %cf, align 1, !revenant.va !1849
  %o260 = xor i32 %t252, 2, !revenant.va !1849
  %n261 = xor i32 %o260, -1, !revenant.va !1849
  %o2262 = xor i32 %t252, %66, !revenant.va !1849
  %o3263 = and i32 %n261, %o2262, !revenant.va !1849
  %o4264 = and i32 %o3263, -2147483648, !revenant.va !1849
  %of265 = icmp ne i32 %o4264, 0, !revenant.va !1849
  store i1 %of265, ptr %of, align 1, !revenant.va !1849
  %z266 = zext i32 %66 to i64, !revenant.va !1849
  store i64 %z266, ptr %rcx, align 8, !revenant.va !1849
  %r267 = load i64, ptr %rcx, align 8, !revenant.va !1850
  %e268 = trunc i64 %r267 to i32, !revenant.va !1850
  %c269 = call i32 @sub_1400013C0(i32 %e268), !revenant.va !1850
  store i64 undef, ptr %rax, align 8, !revenant.va !1850
  store i64 undef, ptr %rcx, align 8, !revenant.va !1850
  store i64 undef, ptr %rdx, align 8, !revenant.va !1850
  store i64 undef, ptr %r8, align 8, !revenant.va !1850
  store i64 undef, ptr %r9, align 8, !revenant.va !1850
  store i64 undef, ptr %r10, align 8, !revenant.va !1850
  store i64 undef, ptr %r11, align 8, !revenant.va !1850
  store i128 undef, ptr %xmm0, align 16, !revenant.va !1850
  store i128 undef, ptr %xmm1, align 16, !revenant.va !1850
  store i128 undef, ptr %xmm2, align 16, !revenant.va !1850
  store i128 undef, ptr %xmm3, align 16, !revenant.va !1850
  store i128 undef, ptr %xmm4, align 16, !revenant.va !1850
  store i128 undef, ptr %xmm5, align 16, !revenant.va !1850
  store i1 undef, ptr %cf, align 1, !revenant.va !1850
  store i1 undef, ptr %of, align 1, !revenant.va !1850
  store i1 undef, ptr %pf, align 1, !revenant.va !1850
  store i1 undef, ptr %sf, align 1, !revenant.va !1850
  store i1 undef, ptr %zf, align 1, !revenant.va !1850
  %e270 = zext i32 %c269 to i64, !revenant.va !1850
  store i64 %e270, ptr %rax, align 8, !revenant.va !1850
  %fp271 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1851
  %68 = ptrtoint ptr %fp271 to i64, !revenant.va !1851
  %69 = load i32, ptr %fp271, align 1, !revenant.va !1851
  %r272 = load i64, ptr %rax, align 8, !revenant.va !1851
  %t273 = trunc i64 %r272 to i32, !revenant.va !1851
  %70 = add i32 %t273, %69, !revenant.va !1851
  %zf274 = icmp eq i32 %70, 0, !revenant.va !1851
  store i1 %zf274, ptr %zf, align 1, !revenant.va !1851
  %sf275 = and i32 %70, -2147483648, !revenant.va !1851
  %sf276 = icmp ne i32 %sf275, 0, !revenant.va !1851
  store i1 %sf276, ptr %sf, align 1, !revenant.va !1851
  %l277 = trunc i32 %70 to i8, !revenant.va !1851
  %71 = call i8 @llvm.ctpop.i8(i8 %l277), !revenant.va !1851
  %pf278 = trunc i8 %71 to i1, !revenant.va !1851
  %pf279 = xor i1 %pf278, true, !revenant.va !1851
  store i1 %pf279, ptr %pf, align 1, !revenant.va !1851
  %cf280 = icmp ult i32 %70, %t273, !revenant.va !1851
  store i1 %cf280, ptr %cf, align 1, !revenant.va !1851
  %o281 = xor i32 %t273, %69, !revenant.va !1851
  %n282 = xor i32 %o281, -1, !revenant.va !1851
  %o2283 = xor i32 %t273, %70, !revenant.va !1851
  %o3284 = and i32 %n282, %o2283, !revenant.va !1851
  %o4285 = and i32 %o3284, -2147483648, !revenant.va !1851
  %of286 = icmp ne i32 %o4285, 0, !revenant.va !1851
  store i1 %of286, ptr %of, align 1, !revenant.va !1851
  %z287 = zext i32 %70 to i64, !revenant.va !1851
  store i64 %z287, ptr %rax, align 8, !revenant.va !1851
  %r288 = load i64, ptr %rax, align 8, !revenant.va !1852
  %t289 = trunc i64 %r288 to i32, !revenant.va !1852
  %fp290 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1852
  %72 = ptrtoint ptr %fp290 to i64, !revenant.va !1852
  store i32 %t289, ptr %fp290, align 1, !revenant.va !1852
  %fp291 = getelementptr inbounds i8, ptr %sp0, i64 -316, !revenant.va !1853
  %73 = ptrtoint ptr %fp291 to i64, !revenant.va !1853
  %74 = load i32, ptr %fp291, align 1, !revenant.va !1853
  %z292 = zext i32 %74 to i64, !revenant.va !1853
  store i64 %z292, ptr %rcx, align 8, !revenant.va !1853
  %r293 = load i64, ptr %rcx, align 8, !revenant.va !1854
  %e294 = trunc i64 %r293 to i32, !revenant.va !1854
  %c295 = call i32 @sub_140001470(i32 %e294), !revenant.va !1854
  store i64 undef, ptr %rax, align 8, !revenant.va !1854
  store i64 undef, ptr %rcx, align 8, !revenant.va !1854
  store i64 undef, ptr %rdx, align 8, !revenant.va !1854
  store i64 undef, ptr %r8, align 8, !revenant.va !1854
  store i64 undef, ptr %r9, align 8, !revenant.va !1854
  store i64 undef, ptr %r10, align 8, !revenant.va !1854
  store i64 undef, ptr %r11, align 8, !revenant.va !1854
  store i128 undef, ptr %xmm0, align 16, !revenant.va !1854
  store i128 undef, ptr %xmm1, align 16, !revenant.va !1854
  store i128 undef, ptr %xmm2, align 16, !revenant.va !1854
  store i128 undef, ptr %xmm3, align 16, !revenant.va !1854
  store i128 undef, ptr %xmm4, align 16, !revenant.va !1854
  store i128 undef, ptr %xmm5, align 16, !revenant.va !1854
  store i1 undef, ptr %cf, align 1, !revenant.va !1854
  store i1 undef, ptr %of, align 1, !revenant.va !1854
  store i1 undef, ptr %pf, align 1, !revenant.va !1854
  store i1 undef, ptr %sf, align 1, !revenant.va !1854
  store i1 undef, ptr %zf, align 1, !revenant.va !1854
  %e296 = zext i32 %c295 to i64, !revenant.va !1854
  store i64 %e296, ptr %rax, align 8, !revenant.va !1854
  %fp297 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1855
  %75 = ptrtoint ptr %fp297 to i64, !revenant.va !1855
  %76 = load i32, ptr %fp297, align 1, !revenant.va !1855
  %r298 = load i64, ptr %rax, align 8, !revenant.va !1855
  %t299 = trunc i64 %r298 to i32, !revenant.va !1855
  %77 = add i32 %t299, %76, !revenant.va !1855
  %zf300 = icmp eq i32 %77, 0, !revenant.va !1855
  store i1 %zf300, ptr %zf, align 1, !revenant.va !1855
  %sf301 = and i32 %77, -2147483648, !revenant.va !1855
  %sf302 = icmp ne i32 %sf301, 0, !revenant.va !1855
  store i1 %sf302, ptr %sf, align 1, !revenant.va !1855
  %l303 = trunc i32 %77 to i8, !revenant.va !1855
  %78 = call i8 @llvm.ctpop.i8(i8 %l303), !revenant.va !1855
  %pf304 = trunc i8 %78 to i1, !revenant.va !1855
  %pf305 = xor i1 %pf304, true, !revenant.va !1855
  store i1 %pf305, ptr %pf, align 1, !revenant.va !1855
  %cf306 = icmp ult i32 %77, %t299, !revenant.va !1855
  store i1 %cf306, ptr %cf, align 1, !revenant.va !1855
  %o307 = xor i32 %t299, %76, !revenant.va !1855
  %n308 = xor i32 %o307, -1, !revenant.va !1855
  %o2309 = xor i32 %t299, %77, !revenant.va !1855
  %o3310 = and i32 %n308, %o2309, !revenant.va !1855
  %o4311 = and i32 %o3310, -2147483648, !revenant.va !1855
  %of312 = icmp ne i32 %o4311, 0, !revenant.va !1855
  store i1 %of312, ptr %of, align 1, !revenant.va !1855
  %z313 = zext i32 %77 to i64, !revenant.va !1855
  store i64 %z313, ptr %rax, align 8, !revenant.va !1855
  %r314 = load i64, ptr %rax, align 8, !revenant.va !1856
  %t315 = trunc i64 %r314 to i32, !revenant.va !1856
  %fp316 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1856
  %79 = ptrtoint ptr %fp316 to i64, !revenant.va !1856
  store i32 %t315, ptr %fp316, align 1, !revenant.va !1856
  %fp317 = getelementptr inbounds i8, ptr %sp0, i64 -316, !revenant.va !1857
  %80 = ptrtoint ptr %fp317 to i64, !revenant.va !1857
  %81 = load i32, ptr %fp317, align 1, !revenant.va !1857
  %z318 = zext i32 %81 to i64, !revenant.va !1857
  store i64 %z318, ptr %rcx, align 8, !revenant.va !1857
  store i64 7, ptr %rdx, align 8, !revenant.va !1858
  %r319 = load i64, ptr %rcx, align 8, !revenant.va !1859
  %e320 = trunc i64 %r319 to i32, !revenant.va !1859
  %r321 = load i64, ptr %rdx, align 8, !revenant.va !1859
  %e322 = trunc i64 %r321 to i32, !revenant.va !1859
  %c323 = call i32 @sub_1400014E0(i32 %e320, i32 %e322), !revenant.va !1859
  store i64 undef, ptr %rax, align 8, !revenant.va !1859
  store i64 undef, ptr %rcx, align 8, !revenant.va !1859
  store i1 undef, ptr %cf, align 1, !revenant.va !1859
  store i1 undef, ptr %of, align 1, !revenant.va !1859
  store i1 undef, ptr %pf, align 1, !revenant.va !1859
  store i1 undef, ptr %sf, align 1, !revenant.va !1859
  store i1 undef, ptr %zf, align 1, !revenant.va !1859
  %e324 = zext i32 %c323 to i64, !revenant.va !1859
  store i64 %e324, ptr %rax, align 8, !revenant.va !1859
  %fp325 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1860
  %82 = ptrtoint ptr %fp325 to i64, !revenant.va !1860
  %83 = load i32, ptr %fp325, align 1, !revenant.va !1860
  %r326 = load i64, ptr %rax, align 8, !revenant.va !1860
  %t327 = trunc i64 %r326 to i32, !revenant.va !1860
  %84 = add i32 %t327, %83, !revenant.va !1860
  %zf328 = icmp eq i32 %84, 0, !revenant.va !1860
  store i1 %zf328, ptr %zf, align 1, !revenant.va !1860
  %sf329 = and i32 %84, -2147483648, !revenant.va !1860
  %sf330 = icmp ne i32 %sf329, 0, !revenant.va !1860
  store i1 %sf330, ptr %sf, align 1, !revenant.va !1860
  %l331 = trunc i32 %84 to i8, !revenant.va !1860
  %85 = call i8 @llvm.ctpop.i8(i8 %l331), !revenant.va !1860
  %pf332 = trunc i8 %85 to i1, !revenant.va !1860
  %pf333 = xor i1 %pf332, true, !revenant.va !1860
  store i1 %pf333, ptr %pf, align 1, !revenant.va !1860
  %cf334 = icmp ult i32 %84, %t327, !revenant.va !1860
  store i1 %cf334, ptr %cf, align 1, !revenant.va !1860
  %o335 = xor i32 %t327, %83, !revenant.va !1860
  %n336 = xor i32 %o335, -1, !revenant.va !1860
  %o2337 = xor i32 %t327, %84, !revenant.va !1860
  %o3338 = and i32 %n336, %o2337, !revenant.va !1860
  %o4339 = and i32 %o3338, -2147483648, !revenant.va !1860
  %of340 = icmp ne i32 %o4339, 0, !revenant.va !1860
  store i1 %of340, ptr %of, align 1, !revenant.va !1860
  %z341 = zext i32 %84 to i64, !revenant.va !1860
  store i64 %z341, ptr %rax, align 8, !revenant.va !1860
  %r342 = load i64, ptr %rax, align 8, !revenant.va !1861
  %t343 = trunc i64 %r342 to i32, !revenant.va !1861
  %fp344 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1861
  %86 = ptrtoint ptr %fp344 to i64, !revenant.va !1861
  store i32 %t343, ptr %fp344, align 1, !revenant.va !1861
  %fp345 = getelementptr inbounds i8, ptr %sp0, i64 -316, !revenant.va !1862
  %87 = ptrtoint ptr %fp345 to i64, !revenant.va !1862
  %88 = load i32, ptr %fp345, align 1, !revenant.va !1862
  %z346 = zext i32 %88 to i64, !revenant.va !1862
  store i64 %z346, ptr %rcx, align 8, !revenant.va !1862
  %r347 = load i64, ptr %rcx, align 8, !revenant.va !1863
  %e348 = trunc i64 %r347 to i32, !revenant.va !1863
  %c349 = call i32 @sub_140001500(i32 %e348), !revenant.va !1863
  store i64 undef, ptr %rax, align 8, !revenant.va !1863
  store i1 undef, ptr %cf, align 1, !revenant.va !1863
  store i1 undef, ptr %of, align 1, !revenant.va !1863
  store i1 undef, ptr %pf, align 1, !revenant.va !1863
  store i1 undef, ptr %sf, align 1, !revenant.va !1863
  store i1 undef, ptr %zf, align 1, !revenant.va !1863
  %e350 = zext i32 %c349 to i64, !revenant.va !1863
  store i64 %e350, ptr %rax, align 8, !revenant.va !1863
  %fp351 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1864
  %89 = ptrtoint ptr %fp351 to i64, !revenant.va !1864
  %90 = load i32, ptr %fp351, align 1, !revenant.va !1864
  %r352 = load i64, ptr %rax, align 8, !revenant.va !1864
  %t353 = trunc i64 %r352 to i32, !revenant.va !1864
  %91 = add i32 %t353, %90, !revenant.va !1864
  %zf354 = icmp eq i32 %91, 0, !revenant.va !1864
  store i1 %zf354, ptr %zf, align 1, !revenant.va !1864
  %sf355 = and i32 %91, -2147483648, !revenant.va !1864
  %sf356 = icmp ne i32 %sf355, 0, !revenant.va !1864
  store i1 %sf356, ptr %sf, align 1, !revenant.va !1864
  %l357 = trunc i32 %91 to i8, !revenant.va !1864
  %92 = call i8 @llvm.ctpop.i8(i8 %l357), !revenant.va !1864
  %pf358 = trunc i8 %92 to i1, !revenant.va !1864
  %pf359 = xor i1 %pf358, true, !revenant.va !1864
  store i1 %pf359, ptr %pf, align 1, !revenant.va !1864
  %cf360 = icmp ult i32 %91, %t353, !revenant.va !1864
  store i1 %cf360, ptr %cf, align 1, !revenant.va !1864
  %o361 = xor i32 %t353, %90, !revenant.va !1864
  %n362 = xor i32 %o361, -1, !revenant.va !1864
  %o2363 = xor i32 %t353, %91, !revenant.va !1864
  %o3364 = and i32 %n362, %o2363, !revenant.va !1864
  %o4365 = and i32 %o3364, -2147483648, !revenant.va !1864
  %of366 = icmp ne i32 %o4365, 0, !revenant.va !1864
  store i1 %of366, ptr %of, align 1, !revenant.va !1864
  %z367 = zext i32 %91 to i64, !revenant.va !1864
  store i64 %z367, ptr %rax, align 8, !revenant.va !1864
  %r368 = load i64, ptr %rax, align 8, !revenant.va !1865
  %t369 = trunc i64 %r368 to i32, !revenant.va !1865
  %fp370 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1865
  %93 = ptrtoint ptr %fp370 to i64, !revenant.va !1865
  store i32 %t369, ptr %fp370, align 1, !revenant.va !1865
  %94 = load i64, ptr @g_1400710c0, align 1, !revenant.va !1866
  store i64 %94, ptr %rax, align 8, !revenant.va !1866
  %r371 = load i64, ptr %rax, align 8, !revenant.va !1867
  %fp372 = getelementptr inbounds i8, ptr %sp0, i64 -296, !revenant.va !1867
  %95 = ptrtoint ptr %fp372 to i64, !revenant.va !1867
  store i64 %r371, ptr %fp372, align 1, !revenant.va !1867
  %96 = load i64, ptr @g_1400710c8, align 1, !revenant.va !1868
  store i64 %96, ptr %rax, align 8, !revenant.va !1868
  %r373 = load i64, ptr %rax, align 8, !revenant.va !1869
  %fp374 = getelementptr inbounds i8, ptr %sp0, i64 -288, !revenant.va !1869
  %97 = ptrtoint ptr %fp374 to i64, !revenant.va !1869
  store i64 %r373, ptr %fp374, align 1, !revenant.va !1869
  %fp375 = getelementptr inbounds i8, ptr %sp0, i64 -296, !revenant.va !1870
  %98 = ptrtoint ptr %fp375 to i64, !revenant.va !1870
  store i64 %98, ptr %rcx, align 8, !revenant.va !1870
  %r376 = load i64, ptr %rcx, align 8, !revenant.va !1871
  %c377 = call i32 @sub_140001580(i64 %r376), !revenant.va !1871
  store i64 undef, ptr %rax, align 8, !revenant.va !1871
  store i64 undef, ptr %rcx, align 8, !revenant.va !1871
  store i64 undef, ptr %rdx, align 8, !revenant.va !1871
  store i64 undef, ptr %r8, align 8, !revenant.va !1871
  store i64 undef, ptr %r9, align 8, !revenant.va !1871
  store i64 undef, ptr %r10, align 8, !revenant.va !1871
  store i64 undef, ptr %r11, align 8, !revenant.va !1871
  store i128 undef, ptr %xmm0, align 16, !revenant.va !1871
  store i128 undef, ptr %xmm1, align 16, !revenant.va !1871
  store i128 undef, ptr %xmm2, align 16, !revenant.va !1871
  store i128 undef, ptr %xmm3, align 16, !revenant.va !1871
  store i128 undef, ptr %xmm4, align 16, !revenant.va !1871
  store i128 undef, ptr %xmm5, align 16, !revenant.va !1871
  store i1 undef, ptr %cf, align 1, !revenant.va !1871
  store i1 undef, ptr %of, align 1, !revenant.va !1871
  store i1 undef, ptr %pf, align 1, !revenant.va !1871
  store i1 undef, ptr %sf, align 1, !revenant.va !1871
  store i1 undef, ptr %zf, align 1, !revenant.va !1871
  %e378 = zext i32 %c377 to i64, !revenant.va !1871
  store i64 %e378, ptr %rax, align 8, !revenant.va !1871
  %fp379 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1872
  %99 = ptrtoint ptr %fp379 to i64, !revenant.va !1872
  %100 = load i32, ptr %fp379, align 1, !revenant.va !1872
  %r380 = load i64, ptr %rax, align 8, !revenant.va !1872
  %t381 = trunc i64 %r380 to i32, !revenant.va !1872
  %101 = add i32 %t381, %100, !revenant.va !1872
  %zf382 = icmp eq i32 %101, 0, !revenant.va !1872
  store i1 %zf382, ptr %zf, align 1, !revenant.va !1872
  %sf383 = and i32 %101, -2147483648, !revenant.va !1872
  %sf384 = icmp ne i32 %sf383, 0, !revenant.va !1872
  store i1 %sf384, ptr %sf, align 1, !revenant.va !1872
  %l385 = trunc i32 %101 to i8, !revenant.va !1872
  %102 = call i8 @llvm.ctpop.i8(i8 %l385), !revenant.va !1872
  %pf386 = trunc i8 %102 to i1, !revenant.va !1872
  %pf387 = xor i1 %pf386, true, !revenant.va !1872
  store i1 %pf387, ptr %pf, align 1, !revenant.va !1872
  %cf388 = icmp ult i32 %101, %t381, !revenant.va !1872
  store i1 %cf388, ptr %cf, align 1, !revenant.va !1872
  %o389 = xor i32 %t381, %100, !revenant.va !1872
  %n390 = xor i32 %o389, -1, !revenant.va !1872
  %o2391 = xor i32 %t381, %101, !revenant.va !1872
  %o3392 = and i32 %n390, %o2391, !revenant.va !1872
  %o4393 = and i32 %o3392, -2147483648, !revenant.va !1872
  %of394 = icmp ne i32 %o4393, 0, !revenant.va !1872
  store i1 %of394, ptr %of, align 1, !revenant.va !1872
  %z395 = zext i32 %101 to i64, !revenant.va !1872
  store i64 %z395, ptr %rax, align 8, !revenant.va !1872
  %r396 = load i64, ptr %rax, align 8, !revenant.va !1873
  %t397 = trunc i64 %r396 to i32, !revenant.va !1873
  %fp398 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1873
  %103 = ptrtoint ptr %fp398 to i64, !revenant.va !1873
  store i32 %t397, ptr %fp398, align 1, !revenant.va !1873
  store i64 5, ptr %rcx, align 8, !revenant.va !1874
  %r399 = load i64, ptr %rcx, align 8, !revenant.va !1875
  %e400 = trunc i64 %r399 to i32, !revenant.va !1875
  %c401 = call i32 @sub_1400015F0(i32 %e400), !revenant.va !1875
  store i64 undef, ptr %rax, align 8, !revenant.va !1875
  store i64 undef, ptr %rcx, align 8, !revenant.va !1875
  store i64 undef, ptr %rdx, align 8, !revenant.va !1875
  store i64 undef, ptr %r8, align 8, !revenant.va !1875
  store i64 undef, ptr %r9, align 8, !revenant.va !1875
  store i64 undef, ptr %r10, align 8, !revenant.va !1875
  store i64 undef, ptr %r11, align 8, !revenant.va !1875
  store i128 undef, ptr %xmm0, align 16, !revenant.va !1875
  store i128 undef, ptr %xmm1, align 16, !revenant.va !1875
  store i128 undef, ptr %xmm2, align 16, !revenant.va !1875
  store i128 undef, ptr %xmm3, align 16, !revenant.va !1875
  store i128 undef, ptr %xmm4, align 16, !revenant.va !1875
  store i128 undef, ptr %xmm5, align 16, !revenant.va !1875
  store i1 undef, ptr %cf, align 1, !revenant.va !1875
  store i1 undef, ptr %of, align 1, !revenant.va !1875
  store i1 undef, ptr %pf, align 1, !revenant.va !1875
  store i1 undef, ptr %sf, align 1, !revenant.va !1875
  store i1 undef, ptr %zf, align 1, !revenant.va !1875
  %e402 = zext i32 %c401 to i64, !revenant.va !1875
  store i64 %e402, ptr %rax, align 8, !revenant.va !1875
  %fp403 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1876
  %104 = ptrtoint ptr %fp403 to i64, !revenant.va !1876
  %105 = load i32, ptr %fp403, align 1, !revenant.va !1876
  %r404 = load i64, ptr %rax, align 8, !revenant.va !1876
  %t405 = trunc i64 %r404 to i32, !revenant.va !1876
  %106 = add i32 %t405, %105, !revenant.va !1876
  %zf406 = icmp eq i32 %106, 0, !revenant.va !1876
  store i1 %zf406, ptr %zf, align 1, !revenant.va !1876
  %sf407 = and i32 %106, -2147483648, !revenant.va !1876
  %sf408 = icmp ne i32 %sf407, 0, !revenant.va !1876
  store i1 %sf408, ptr %sf, align 1, !revenant.va !1876
  %l409 = trunc i32 %106 to i8, !revenant.va !1876
  %107 = call i8 @llvm.ctpop.i8(i8 %l409), !revenant.va !1876
  %pf410 = trunc i8 %107 to i1, !revenant.va !1876
  %pf411 = xor i1 %pf410, true, !revenant.va !1876
  store i1 %pf411, ptr %pf, align 1, !revenant.va !1876
  %cf412 = icmp ult i32 %106, %t405, !revenant.va !1876
  store i1 %cf412, ptr %cf, align 1, !revenant.va !1876
  %o413 = xor i32 %t405, %105, !revenant.va !1876
  %n414 = xor i32 %o413, -1, !revenant.va !1876
  %o2415 = xor i32 %t405, %106, !revenant.va !1876
  %o3416 = and i32 %n414, %o2415, !revenant.va !1876
  %o4417 = and i32 %o3416, -2147483648, !revenant.va !1876
  %of418 = icmp ne i32 %o4417, 0, !revenant.va !1876
  store i1 %of418, ptr %of, align 1, !revenant.va !1876
  %z419 = zext i32 %106 to i64, !revenant.va !1876
  store i64 %z419, ptr %rax, align 8, !revenant.va !1876
  %r420 = load i64, ptr %rax, align 8, !revenant.va !1877
  %t421 = trunc i64 %r420 to i32, !revenant.va !1877
  %fp422 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1877
  %108 = ptrtoint ptr %fp422 to i64, !revenant.va !1877
  store i32 %t421, ptr %fp422, align 1, !revenant.va !1877
  store i64 10, ptr %rcx, align 8, !revenant.va !1878
  %r423 = load i64, ptr %rcx, align 8, !revenant.va !1879
  %e424 = trunc i64 %r423 to i32, !revenant.va !1879
  %c425 = call i32 @sub_1400016A0(i32 %e424), !revenant.va !1879
  store i64 undef, ptr %rax, align 8, !revenant.va !1879
  store i64 undef, ptr %rcx, align 8, !revenant.va !1879
  store i64 undef, ptr %rdx, align 8, !revenant.va !1879
  store i64 undef, ptr %r8, align 8, !revenant.va !1879
  store i64 undef, ptr %r9, align 8, !revenant.va !1879
  store i64 undef, ptr %r10, align 8, !revenant.va !1879
  store i64 undef, ptr %r11, align 8, !revenant.va !1879
  store i128 undef, ptr %xmm0, align 16, !revenant.va !1879
  store i128 undef, ptr %xmm1, align 16, !revenant.va !1879
  store i128 undef, ptr %xmm2, align 16, !revenant.va !1879
  store i128 undef, ptr %xmm3, align 16, !revenant.va !1879
  store i128 undef, ptr %xmm4, align 16, !revenant.va !1879
  store i128 undef, ptr %xmm5, align 16, !revenant.va !1879
  store i1 undef, ptr %cf, align 1, !revenant.va !1879
  store i1 undef, ptr %of, align 1, !revenant.va !1879
  store i1 undef, ptr %pf, align 1, !revenant.va !1879
  store i1 undef, ptr %sf, align 1, !revenant.va !1879
  store i1 undef, ptr %zf, align 1, !revenant.va !1879
  %e426 = zext i32 %c425 to i64, !revenant.va !1879
  store i64 %e426, ptr %rax, align 8, !revenant.va !1879
  %fp427 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1880
  %109 = ptrtoint ptr %fp427 to i64, !revenant.va !1880
  %110 = load i32, ptr %fp427, align 1, !revenant.va !1880
  %r428 = load i64, ptr %rax, align 8, !revenant.va !1880
  %t429 = trunc i64 %r428 to i32, !revenant.va !1880
  %111 = add i32 %t429, %110, !revenant.va !1880
  %zf430 = icmp eq i32 %111, 0, !revenant.va !1880
  store i1 %zf430, ptr %zf, align 1, !revenant.va !1880
  %sf431 = and i32 %111, -2147483648, !revenant.va !1880
  %sf432 = icmp ne i32 %sf431, 0, !revenant.va !1880
  store i1 %sf432, ptr %sf, align 1, !revenant.va !1880
  %l433 = trunc i32 %111 to i8, !revenant.va !1880
  %112 = call i8 @llvm.ctpop.i8(i8 %l433), !revenant.va !1880
  %pf434 = trunc i8 %112 to i1, !revenant.va !1880
  %pf435 = xor i1 %pf434, true, !revenant.va !1880
  store i1 %pf435, ptr %pf, align 1, !revenant.va !1880
  %cf436 = icmp ult i32 %111, %t429, !revenant.va !1880
  store i1 %cf436, ptr %cf, align 1, !revenant.va !1880
  %o437 = xor i32 %t429, %110, !revenant.va !1880
  %n438 = xor i32 %o437, -1, !revenant.va !1880
  %o2439 = xor i32 %t429, %111, !revenant.va !1880
  %o3440 = and i32 %n438, %o2439, !revenant.va !1880
  %o4441 = and i32 %o3440, -2147483648, !revenant.va !1880
  %of442 = icmp ne i32 %o4441, 0, !revenant.va !1880
  store i1 %of442, ptr %of, align 1, !revenant.va !1880
  %z443 = zext i32 %111 to i64, !revenant.va !1880
  store i64 %z443, ptr %rax, align 8, !revenant.va !1880
  %r444 = load i64, ptr %rax, align 8, !revenant.va !1881
  %t445 = trunc i64 %r444 to i32, !revenant.va !1881
  %fp446 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1881
  %113 = ptrtoint ptr %fp446 to i64, !revenant.va !1881
  store i32 %t445, ptr %fp446, align 1, !revenant.va !1881
  %r447 = load i64, ptr %rcx, align 8, !revenant.va !1882
  %t448 = trunc i64 %r447 to i32, !revenant.va !1882
  %r449 = load i64, ptr %rcx, align 8, !revenant.va !1882
  %t450 = trunc i64 %r449 to i32, !revenant.va !1882
  %114 = xor i32 %t450, %t448, !revenant.va !1882
  %zf451 = icmp eq i32 %114, 0, !revenant.va !1882
  store i1 %zf451, ptr %zf, align 1, !revenant.va !1882
  %sf452 = and i32 %114, -2147483648, !revenant.va !1882
  %sf453 = icmp ne i32 %sf452, 0, !revenant.va !1882
  store i1 %sf453, ptr %sf, align 1, !revenant.va !1882
  %l454 = trunc i32 %114 to i8, !revenant.va !1882
  %115 = call i8 @llvm.ctpop.i8(i8 %l454), !revenant.va !1882
  %pf455 = trunc i8 %115 to i1, !revenant.va !1882
  %pf456 = xor i1 %pf455, true, !revenant.va !1882
  store i1 %pf456, ptr %pf, align 1, !revenant.va !1882
  store i1 false, ptr %cf, align 1, !revenant.va !1882
  store i1 false, ptr %of, align 1, !revenant.va !1882
  %z457 = zext i32 %114 to i64, !revenant.va !1882
  store i64 %z457, ptr %rcx, align 8, !revenant.va !1882
  store i64 9, ptr %rdx, align 8, !revenant.va !1883
  store i64 4, ptr %r8, align 8, !revenant.va !1884
  %r458 = load i64, ptr %rcx, align 8, !revenant.va !1885
  %e459 = trunc i64 %r458 to i32, !revenant.va !1885
  %r460 = load i64, ptr %rdx, align 8, !revenant.va !1885
  %e461 = trunc i64 %r460 to i32, !revenant.va !1885
  %r462 = load i64, ptr %r8, align 8, !revenant.va !1885
  %e463 = trunc i64 %r462 to i32, !revenant.va !1885
  %c464 = call i32 @sub_1400016F0(i32 %e459, i32 %e461, i32 %e463), !revenant.va !1885
  store i64 undef, ptr %rax, align 8, !revenant.va !1885
  store i64 undef, ptr %rcx, align 8, !revenant.va !1885
  store i64 undef, ptr %rdx, align 8, !revenant.va !1885
  store i64 undef, ptr %r8, align 8, !revenant.va !1885
  store i64 undef, ptr %r9, align 8, !revenant.va !1885
  store i64 undef, ptr %r10, align 8, !revenant.va !1885
  store i64 undef, ptr %r11, align 8, !revenant.va !1885
  store i128 undef, ptr %xmm0, align 16, !revenant.va !1885
  store i128 undef, ptr %xmm1, align 16, !revenant.va !1885
  store i128 undef, ptr %xmm2, align 16, !revenant.va !1885
  store i128 undef, ptr %xmm3, align 16, !revenant.va !1885
  store i128 undef, ptr %xmm4, align 16, !revenant.va !1885
  store i128 undef, ptr %xmm5, align 16, !revenant.va !1885
  store i1 undef, ptr %cf, align 1, !revenant.va !1885
  store i1 undef, ptr %of, align 1, !revenant.va !1885
  store i1 undef, ptr %pf, align 1, !revenant.va !1885
  store i1 undef, ptr %sf, align 1, !revenant.va !1885
  store i1 undef, ptr %zf, align 1, !revenant.va !1885
  %e465 = zext i32 %c464 to i64, !revenant.va !1885
  store i64 %e465, ptr %rax, align 8, !revenant.va !1885
  %fp466 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1886
  %116 = ptrtoint ptr %fp466 to i64, !revenant.va !1886
  %117 = load i32, ptr %fp466, align 1, !revenant.va !1886
  %r467 = load i64, ptr %rax, align 8, !revenant.va !1886
  %t468 = trunc i64 %r467 to i32, !revenant.va !1886
  %118 = add i32 %t468, %117, !revenant.va !1886
  %zf469 = icmp eq i32 %118, 0, !revenant.va !1886
  store i1 %zf469, ptr %zf, align 1, !revenant.va !1886
  %sf470 = and i32 %118, -2147483648, !revenant.va !1886
  %sf471 = icmp ne i32 %sf470, 0, !revenant.va !1886
  store i1 %sf471, ptr %sf, align 1, !revenant.va !1886
  %l472 = trunc i32 %118 to i8, !revenant.va !1886
  %119 = call i8 @llvm.ctpop.i8(i8 %l472), !revenant.va !1886
  %pf473 = trunc i8 %119 to i1, !revenant.va !1886
  %pf474 = xor i1 %pf473, true, !revenant.va !1886
  store i1 %pf474, ptr %pf, align 1, !revenant.va !1886
  %cf475 = icmp ult i32 %118, %t468, !revenant.va !1886
  store i1 %cf475, ptr %cf, align 1, !revenant.va !1886
  %o476 = xor i32 %t468, %117, !revenant.va !1886
  %n477 = xor i32 %o476, -1, !revenant.va !1886
  %o2478 = xor i32 %t468, %118, !revenant.va !1886
  %o3479 = and i32 %n477, %o2478, !revenant.va !1886
  %o4480 = and i32 %o3479, -2147483648, !revenant.va !1886
  %of481 = icmp ne i32 %o4480, 0, !revenant.va !1886
  store i1 %of481, ptr %of, align 1, !revenant.va !1886
  %z482 = zext i32 %118 to i64, !revenant.va !1886
  store i64 %z482, ptr %rax, align 8, !revenant.va !1886
  %r483 = load i64, ptr %rax, align 8, !revenant.va !1887
  %t484 = trunc i64 %r483 to i32, !revenant.va !1887
  %fp485 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1887
  %120 = ptrtoint ptr %fp485 to i64, !revenant.va !1887
  store i32 %t484, ptr %fp485, align 1, !revenant.va !1887
  %fp486 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1888
  %121 = ptrtoint ptr %fp486 to i64, !revenant.va !1888
  %122 = load i32, ptr %fp486, align 1, !revenant.va !1888
  %z487 = zext i32 %122 to i64, !revenant.va !1888
  store i64 %z487, ptr %rcx, align 8, !revenant.va !1888
  %r488 = load i64, ptr %rcx, align 8, !revenant.va !1889
  %e489 = trunc i64 %r488 to i32, !revenant.va !1889
  %c490 = call i32 @sub_1400017B0(i32 %e489), !revenant.va !1889
  store i64 undef, ptr %rax, align 8, !revenant.va !1889
  store i64 undef, ptr %rcx, align 8, !revenant.va !1889
  store i64 undef, ptr %rdx, align 8, !revenant.va !1889
  store i1 undef, ptr %cf, align 1, !revenant.va !1889
  store i1 undef, ptr %of, align 1, !revenant.va !1889
  store i1 undef, ptr %pf, align 1, !revenant.va !1889
  store i1 undef, ptr %sf, align 1, !revenant.va !1889
  store i1 undef, ptr %zf, align 1, !revenant.va !1889
  %e491 = zext i32 %c490 to i64, !revenant.va !1889
  store i64 %e491, ptr %rax, align 8, !revenant.va !1889
  %fp492 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1890
  %123 = ptrtoint ptr %fp492 to i64, !revenant.va !1890
  %124 = load i32, ptr %fp492, align 1, !revenant.va !1890
  %r493 = load i64, ptr %rax, align 8, !revenant.va !1890
  %t494 = trunc i64 %r493 to i32, !revenant.va !1890
  %125 = add i32 %t494, %124, !revenant.va !1890
  %zf495 = icmp eq i32 %125, 0, !revenant.va !1890
  store i1 %zf495, ptr %zf, align 1, !revenant.va !1890
  %sf496 = and i32 %125, -2147483648, !revenant.va !1890
  %sf497 = icmp ne i32 %sf496, 0, !revenant.va !1890
  store i1 %sf497, ptr %sf, align 1, !revenant.va !1890
  %l498 = trunc i32 %125 to i8, !revenant.va !1890
  %126 = call i8 @llvm.ctpop.i8(i8 %l498), !revenant.va !1890
  %pf499 = trunc i8 %126 to i1, !revenant.va !1890
  %pf500 = xor i1 %pf499, true, !revenant.va !1890
  store i1 %pf500, ptr %pf, align 1, !revenant.va !1890
  %cf501 = icmp ult i32 %125, %t494, !revenant.va !1890
  store i1 %cf501, ptr %cf, align 1, !revenant.va !1890
  %o502 = xor i32 %t494, %124, !revenant.va !1890
  %n503 = xor i32 %o502, -1, !revenant.va !1890
  %o2504 = xor i32 %t494, %125, !revenant.va !1890
  %o3505 = and i32 %n503, %o2504, !revenant.va !1890
  %o4506 = and i32 %o3505, -2147483648, !revenant.va !1890
  %of507 = icmp ne i32 %o4506, 0, !revenant.va !1890
  store i1 %of507, ptr %of, align 1, !revenant.va !1890
  %z508 = zext i32 %125 to i64, !revenant.va !1890
  store i64 %z508, ptr %rax, align 8, !revenant.va !1890
  %r509 = load i64, ptr %rax, align 8, !revenant.va !1891
  %t510 = trunc i64 %r509 to i32, !revenant.va !1891
  %fp511 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1891
  %127 = ptrtoint ptr %fp511 to i64, !revenant.va !1891
  store i32 %t510, ptr %fp511, align 1, !revenant.va !1891
  %fp512 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1892
  %128 = ptrtoint ptr %fp512 to i64, !revenant.va !1892
  %129 = load i32, ptr %fp512, align 1, !revenant.va !1892
  %z513 = zext i32 %129 to i64, !revenant.va !1892
  store i64 %z513, ptr %rcx, align 8, !revenant.va !1892
  %r514 = load i64, ptr %rcx, align 8, !revenant.va !1893
  %e515 = trunc i64 %r514 to i32, !revenant.va !1893
  %c516 = call i32 @sub_1400017D0(i32 %e515), !revenant.va !1893
  store i64 undef, ptr %rax, align 8, !revenant.va !1893
  store i64 undef, ptr %rcx, align 8, !revenant.va !1893
  store i64 undef, ptr %rdx, align 8, !revenant.va !1893
  store i1 undef, ptr %cf, align 1, !revenant.va !1893
  store i1 undef, ptr %of, align 1, !revenant.va !1893
  store i1 undef, ptr %pf, align 1, !revenant.va !1893
  store i1 undef, ptr %sf, align 1, !revenant.va !1893
  store i1 undef, ptr %zf, align 1, !revenant.va !1893
  %e517 = zext i32 %c516 to i64, !revenant.va !1893
  store i64 %e517, ptr %rax, align 8, !revenant.va !1893
  %fp518 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1894
  %130 = ptrtoint ptr %fp518 to i64, !revenant.va !1894
  %131 = load i32, ptr %fp518, align 1, !revenant.va !1894
  %r519 = load i64, ptr %rax, align 8, !revenant.va !1894
  %t520 = trunc i64 %r519 to i32, !revenant.va !1894
  %132 = add i32 %t520, %131, !revenant.va !1894
  %zf521 = icmp eq i32 %132, 0, !revenant.va !1894
  store i1 %zf521, ptr %zf, align 1, !revenant.va !1894
  %sf522 = and i32 %132, -2147483648, !revenant.va !1894
  %sf523 = icmp ne i32 %sf522, 0, !revenant.va !1894
  store i1 %sf523, ptr %sf, align 1, !revenant.va !1894
  %l524 = trunc i32 %132 to i8, !revenant.va !1894
  %133 = call i8 @llvm.ctpop.i8(i8 %l524), !revenant.va !1894
  %pf525 = trunc i8 %133 to i1, !revenant.va !1894
  %pf526 = xor i1 %pf525, true, !revenant.va !1894
  store i1 %pf526, ptr %pf, align 1, !revenant.va !1894
  %cf527 = icmp ult i32 %132, %t520, !revenant.va !1894
  store i1 %cf527, ptr %cf, align 1, !revenant.va !1894
  %o528 = xor i32 %t520, %131, !revenant.va !1894
  %n529 = xor i32 %o528, -1, !revenant.va !1894
  %o2530 = xor i32 %t520, %132, !revenant.va !1894
  %o3531 = and i32 %n529, %o2530, !revenant.va !1894
  %o4532 = and i32 %o3531, -2147483648, !revenant.va !1894
  %of533 = icmp ne i32 %o4532, 0, !revenant.va !1894
  store i1 %of533, ptr %of, align 1, !revenant.va !1894
  %z534 = zext i32 %132 to i64, !revenant.va !1894
  store i64 %z534, ptr %rax, align 8, !revenant.va !1894
  %r535 = load i64, ptr %rax, align 8, !revenant.va !1895
  %t536 = trunc i64 %r535 to i32, !revenant.va !1895
  %fp537 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1895
  %134 = ptrtoint ptr %fp537 to i64, !revenant.va !1895
  store i32 %t536, ptr %fp537, align 1, !revenant.va !1895
  %fp538 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1896
  %135 = ptrtoint ptr %fp538 to i64, !revenant.va !1896
  %136 = load i32, ptr %fp538, align 1, !revenant.va !1896
  %z539 = zext i32 %136 to i64, !revenant.va !1896
  store i64 %z539, ptr %rcx, align 8, !revenant.va !1896
  %r540 = load i64, ptr %rcx, align 8, !revenant.va !1897
  %e541 = trunc i64 %r540 to i32, !revenant.va !1897
  %c542 = call i32 @sub_1400017F0(i32 %e541), !revenant.va !1897
  store i64 undef, ptr %rax, align 8, !revenant.va !1897
  store i64 undef, ptr %rcx, align 8, !revenant.va !1897
  store i64 undef, ptr %rdx, align 8, !revenant.va !1897
  store i1 undef, ptr %cf, align 1, !revenant.va !1897
  store i1 undef, ptr %of, align 1, !revenant.va !1897
  store i1 undef, ptr %pf, align 1, !revenant.va !1897
  store i1 undef, ptr %sf, align 1, !revenant.va !1897
  store i1 undef, ptr %zf, align 1, !revenant.va !1897
  %e543 = zext i32 %c542 to i64, !revenant.va !1897
  store i64 %e543, ptr %rax, align 8, !revenant.va !1897
  %fp544 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1898
  %137 = ptrtoint ptr %fp544 to i64, !revenant.va !1898
  %138 = load i32, ptr %fp544, align 1, !revenant.va !1898
  %r545 = load i64, ptr %rax, align 8, !revenant.va !1898
  %t546 = trunc i64 %r545 to i32, !revenant.va !1898
  %139 = add i32 %t546, %138, !revenant.va !1898
  %zf547 = icmp eq i32 %139, 0, !revenant.va !1898
  store i1 %zf547, ptr %zf, align 1, !revenant.va !1898
  %sf548 = and i32 %139, -2147483648, !revenant.va !1898
  %sf549 = icmp ne i32 %sf548, 0, !revenant.va !1898
  store i1 %sf549, ptr %sf, align 1, !revenant.va !1898
  %l550 = trunc i32 %139 to i8, !revenant.va !1898
  %140 = call i8 @llvm.ctpop.i8(i8 %l550), !revenant.va !1898
  %pf551 = trunc i8 %140 to i1, !revenant.va !1898
  %pf552 = xor i1 %pf551, true, !revenant.va !1898
  store i1 %pf552, ptr %pf, align 1, !revenant.va !1898
  %cf553 = icmp ult i32 %139, %t546, !revenant.va !1898
  store i1 %cf553, ptr %cf, align 1, !revenant.va !1898
  %o554 = xor i32 %t546, %138, !revenant.va !1898
  %n555 = xor i32 %o554, -1, !revenant.va !1898
  %o2556 = xor i32 %t546, %139, !revenant.va !1898
  %o3557 = and i32 %n555, %o2556, !revenant.va !1898
  %o4558 = and i32 %o3557, -2147483648, !revenant.va !1898
  %of559 = icmp ne i32 %o4558, 0, !revenant.va !1898
  store i1 %of559, ptr %of, align 1, !revenant.va !1898
  %z560 = zext i32 %139 to i64, !revenant.va !1898
  store i64 %z560, ptr %rax, align 8, !revenant.va !1898
  %r561 = load i64, ptr %rax, align 8, !revenant.va !1899
  %t562 = trunc i64 %r561 to i32, !revenant.va !1899
  %fp563 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1899
  %141 = ptrtoint ptr %fp563 to i64, !revenant.va !1899
  store i32 %t562, ptr %fp563, align 1, !revenant.va !1899
  %fp564 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1900
  %142 = ptrtoint ptr %fp564 to i64, !revenant.va !1900
  %143 = load i32, ptr %fp564, align 1, !revenant.va !1900
  %z565 = zext i32 %143 to i64, !revenant.va !1900
  store i64 %z565, ptr %rcx, align 8, !revenant.va !1900
  %r566 = load i64, ptr %rcx, align 8, !revenant.va !1901
  %e567 = trunc i64 %r566 to i32, !revenant.va !1901
  %c568 = call i32 @sub_140001810(i32 %e567), !revenant.va !1901
  store i64 undef, ptr %rax, align 8, !revenant.va !1901
  store i64 undef, ptr %rcx, align 8, !revenant.va !1901
  store i64 undef, ptr %rdx, align 8, !revenant.va !1901
  store i1 undef, ptr %cf, align 1, !revenant.va !1901
  store i1 undef, ptr %of, align 1, !revenant.va !1901
  store i1 undef, ptr %pf, align 1, !revenant.va !1901
  store i1 undef, ptr %sf, align 1, !revenant.va !1901
  store i1 undef, ptr %zf, align 1, !revenant.va !1901
  %e569 = zext i32 %c568 to i64, !revenant.va !1901
  store i64 %e569, ptr %rax, align 8, !revenant.va !1901
  %fp570 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1902
  %144 = ptrtoint ptr %fp570 to i64, !revenant.va !1902
  %145 = load i32, ptr %fp570, align 1, !revenant.va !1902
  %r571 = load i64, ptr %rax, align 8, !revenant.va !1902
  %t572 = trunc i64 %r571 to i32, !revenant.va !1902
  %146 = add i32 %t572, %145, !revenant.va !1902
  %zf573 = icmp eq i32 %146, 0, !revenant.va !1902
  store i1 %zf573, ptr %zf, align 1, !revenant.va !1902
  %sf574 = and i32 %146, -2147483648, !revenant.va !1902
  %sf575 = icmp ne i32 %sf574, 0, !revenant.va !1902
  store i1 %sf575, ptr %sf, align 1, !revenant.va !1902
  %l576 = trunc i32 %146 to i8, !revenant.va !1902
  %147 = call i8 @llvm.ctpop.i8(i8 %l576), !revenant.va !1902
  %pf577 = trunc i8 %147 to i1, !revenant.va !1902
  %pf578 = xor i1 %pf577, true, !revenant.va !1902
  store i1 %pf578, ptr %pf, align 1, !revenant.va !1902
  %cf579 = icmp ult i32 %146, %t572, !revenant.va !1902
  store i1 %cf579, ptr %cf, align 1, !revenant.va !1902
  %o580 = xor i32 %t572, %145, !revenant.va !1902
  %n581 = xor i32 %o580, -1, !revenant.va !1902
  %o2582 = xor i32 %t572, %146, !revenant.va !1902
  %o3583 = and i32 %n581, %o2582, !revenant.va !1902
  %o4584 = and i32 %o3583, -2147483648, !revenant.va !1902
  %of585 = icmp ne i32 %o4584, 0, !revenant.va !1902
  store i1 %of585, ptr %of, align 1, !revenant.va !1902
  %z586 = zext i32 %146 to i64, !revenant.va !1902
  store i64 %z586, ptr %rax, align 8, !revenant.va !1902
  %r587 = load i64, ptr %rax, align 8, !revenant.va !1903
  %t588 = trunc i64 %r587 to i32, !revenant.va !1903
  %fp589 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1903
  %148 = ptrtoint ptr %fp589 to i64, !revenant.va !1903
  store i32 %t588, ptr %fp589, align 1, !revenant.va !1903
  %fp590 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1904
  %149 = ptrtoint ptr %fp590 to i64, !revenant.va !1904
  %150 = load i32, ptr %fp590, align 1, !revenant.va !1904
  %z591 = zext i32 %150 to i64, !revenant.va !1904
  store i64 %z591, ptr %rcx, align 8, !revenant.va !1904
  %r592 = load i64, ptr %rcx, align 8, !revenant.va !1905
  %e593 = trunc i64 %r592 to i32, !revenant.va !1905
  %c594 = call i32 @sub_140001830(i32 %e593), !revenant.va !1905
  store i64 undef, ptr %rax, align 8, !revenant.va !1905
  store i64 undef, ptr %rcx, align 8, !revenant.va !1905
  store i64 undef, ptr %rdx, align 8, !revenant.va !1905
  store i1 undef, ptr %cf, align 1, !revenant.va !1905
  store i1 undef, ptr %of, align 1, !revenant.va !1905
  store i1 undef, ptr %pf, align 1, !revenant.va !1905
  store i1 undef, ptr %sf, align 1, !revenant.va !1905
  store i1 undef, ptr %zf, align 1, !revenant.va !1905
  %e595 = zext i32 %c594 to i64, !revenant.va !1905
  store i64 %e595, ptr %rax, align 8, !revenant.va !1905
  %fp596 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1906
  %151 = ptrtoint ptr %fp596 to i64, !revenant.va !1906
  %152 = load i32, ptr %fp596, align 1, !revenant.va !1906
  %r597 = load i64, ptr %rax, align 8, !revenant.va !1906
  %t598 = trunc i64 %r597 to i32, !revenant.va !1906
  %153 = add i32 %t598, %152, !revenant.va !1906
  %zf599 = icmp eq i32 %153, 0, !revenant.va !1906
  store i1 %zf599, ptr %zf, align 1, !revenant.va !1906
  %sf600 = and i32 %153, -2147483648, !revenant.va !1906
  %sf601 = icmp ne i32 %sf600, 0, !revenant.va !1906
  store i1 %sf601, ptr %sf, align 1, !revenant.va !1906
  %l602 = trunc i32 %153 to i8, !revenant.va !1906
  %154 = call i8 @llvm.ctpop.i8(i8 %l602), !revenant.va !1906
  %pf603 = trunc i8 %154 to i1, !revenant.va !1906
  %pf604 = xor i1 %pf603, true, !revenant.va !1906
  store i1 %pf604, ptr %pf, align 1, !revenant.va !1906
  %cf605 = icmp ult i32 %153, %t598, !revenant.va !1906
  store i1 %cf605, ptr %cf, align 1, !revenant.va !1906
  %o606 = xor i32 %t598, %152, !revenant.va !1906
  %n607 = xor i32 %o606, -1, !revenant.va !1906
  %o2608 = xor i32 %t598, %153, !revenant.va !1906
  %o3609 = and i32 %n607, %o2608, !revenant.va !1906
  %o4610 = and i32 %o3609, -2147483648, !revenant.va !1906
  %of611 = icmp ne i32 %o4610, 0, !revenant.va !1906
  store i1 %of611, ptr %of, align 1, !revenant.va !1906
  %z612 = zext i32 %153 to i64, !revenant.va !1906
  store i64 %z612, ptr %rax, align 8, !revenant.va !1906
  %r613 = load i64, ptr %rax, align 8, !revenant.va !1907
  %t614 = trunc i64 %r613 to i32, !revenant.va !1907
  %fp615 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1907
  %155 = ptrtoint ptr %fp615 to i64, !revenant.va !1907
  store i32 %t614, ptr %fp615, align 1, !revenant.va !1907
  %fp616 = getelementptr inbounds i8, ptr %sp0, i64 -316, !revenant.va !1908
  %156 = ptrtoint ptr %fp616 to i64, !revenant.va !1908
  %157 = load i32, ptr %fp616, align 1, !revenant.va !1908
  %z617 = zext i32 %157 to i64, !revenant.va !1908
  store i64 %z617, ptr %rcx, align 8, !revenant.va !1908
  store i64 5, ptr %rdx, align 8, !revenant.va !1909
  %r618 = load i64, ptr %rcx, align 8, !revenant.va !1910
  %e619 = trunc i64 %r618 to i32, !revenant.va !1910
  %r620 = load i64, ptr %rdx, align 8, !revenant.va !1910
  %e621 = trunc i64 %r620 to i32, !revenant.va !1910
  %c622 = call i32 @sub_140001850(i32 %e619, i32 %e621), !revenant.va !1910
  store i64 undef, ptr %rax, align 8, !revenant.va !1910
  store i1 undef, ptr %cf, align 1, !revenant.va !1910
  store i1 undef, ptr %of, align 1, !revenant.va !1910
  store i1 undef, ptr %pf, align 1, !revenant.va !1910
  store i1 undef, ptr %sf, align 1, !revenant.va !1910
  store i1 undef, ptr %zf, align 1, !revenant.va !1910
  %e623 = zext i32 %c622 to i64, !revenant.va !1910
  store i64 %e623, ptr %rax, align 8, !revenant.va !1910
  %fp624 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1911
  %158 = ptrtoint ptr %fp624 to i64, !revenant.va !1911
  %159 = load i32, ptr %fp624, align 1, !revenant.va !1911
  %r625 = load i64, ptr %rax, align 8, !revenant.va !1911
  %t626 = trunc i64 %r625 to i32, !revenant.va !1911
  %160 = add i32 %t626, %159, !revenant.va !1911
  %zf627 = icmp eq i32 %160, 0, !revenant.va !1911
  store i1 %zf627, ptr %zf, align 1, !revenant.va !1911
  %sf628 = and i32 %160, -2147483648, !revenant.va !1911
  %sf629 = icmp ne i32 %sf628, 0, !revenant.va !1911
  store i1 %sf629, ptr %sf, align 1, !revenant.va !1911
  %l630 = trunc i32 %160 to i8, !revenant.va !1911
  %161 = call i8 @llvm.ctpop.i8(i8 %l630), !revenant.va !1911
  %pf631 = trunc i8 %161 to i1, !revenant.va !1911
  %pf632 = xor i1 %pf631, true, !revenant.va !1911
  store i1 %pf632, ptr %pf, align 1, !revenant.va !1911
  %cf633 = icmp ult i32 %160, %t626, !revenant.va !1911
  store i1 %cf633, ptr %cf, align 1, !revenant.va !1911
  %o634 = xor i32 %t626, %159, !revenant.va !1911
  %n635 = xor i32 %o634, -1, !revenant.va !1911
  %o2636 = xor i32 %t626, %160, !revenant.va !1911
  %o3637 = and i32 %n635, %o2636, !revenant.va !1911
  %o4638 = and i32 %o3637, -2147483648, !revenant.va !1911
  %of639 = icmp ne i32 %o4638, 0, !revenant.va !1911
  store i1 %of639, ptr %of, align 1, !revenant.va !1911
  %z640 = zext i32 %160 to i64, !revenant.va !1911
  store i64 %z640, ptr %rax, align 8, !revenant.va !1911
  %r641 = load i64, ptr %rax, align 8, !revenant.va !1912
  %t642 = trunc i64 %r641 to i32, !revenant.va !1912
  %fp643 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1912
  %162 = ptrtoint ptr %fp643 to i64, !revenant.va !1912
  store i32 %t642, ptr %fp643, align 1, !revenant.va !1912
  %fp644 = getelementptr inbounds i8, ptr %sp0, i64 -24, !revenant.va !1913
  %163 = ptrtoint ptr %fp644 to i64, !revenant.va !1913
  store i64 %163, ptr %rcx, align 8, !revenant.va !1913
  store i64 171, ptr %rdx, align 8, !revenant.va !1914
  store i64 16, ptr %r8, align 8, !revenant.va !1915
  %r645 = load i64, ptr %rcx, align 8, !revenant.va !1916
  %r646 = load i64, ptr %rdx, align 8, !revenant.va !1916
  %e647 = trunc i64 %r646 to i8, !revenant.va !1916
  %r648 = load i64, ptr %r8, align 8, !revenant.va !1916
  %e649 = trunc i64 %r648 to i32, !revenant.va !1916
  %c650 = call i32 @sub_1400018E0(i64 %r645, i8 %e647, i32 %e649), !revenant.va !1916
  store i64 undef, ptr %rax, align 8, !revenant.va !1916
  store i64 undef, ptr %rcx, align 8, !revenant.va !1916
  store i64 undef, ptr %rdx, align 8, !revenant.va !1916
  store i1 undef, ptr %cf, align 1, !revenant.va !1916
  store i1 undef, ptr %of, align 1, !revenant.va !1916
  store i1 undef, ptr %pf, align 1, !revenant.va !1916
  store i1 undef, ptr %sf, align 1, !revenant.va !1916
  store i1 undef, ptr %zf, align 1, !revenant.va !1916
  %e651 = zext i32 %c650 to i64, !revenant.va !1916
  store i64 %e651, ptr %rax, align 8, !revenant.va !1916
  %fp652 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1917
  %164 = ptrtoint ptr %fp652 to i64, !revenant.va !1917
  %165 = load i32, ptr %fp652, align 1, !revenant.va !1917
  %r653 = load i64, ptr %rax, align 8, !revenant.va !1917
  %t654 = trunc i64 %r653 to i32, !revenant.va !1917
  %166 = add i32 %t654, %165, !revenant.va !1917
  %zf655 = icmp eq i32 %166, 0, !revenant.va !1917
  store i1 %zf655, ptr %zf, align 1, !revenant.va !1917
  %sf656 = and i32 %166, -2147483648, !revenant.va !1917
  %sf657 = icmp ne i32 %sf656, 0, !revenant.va !1917
  store i1 %sf657, ptr %sf, align 1, !revenant.va !1917
  %l658 = trunc i32 %166 to i8, !revenant.va !1917
  %167 = call i8 @llvm.ctpop.i8(i8 %l658), !revenant.va !1917
  %pf659 = trunc i8 %167 to i1, !revenant.va !1917
  %pf660 = xor i1 %pf659, true, !revenant.va !1917
  store i1 %pf660, ptr %pf, align 1, !revenant.va !1917
  %cf661 = icmp ult i32 %166, %t654, !revenant.va !1917
  store i1 %cf661, ptr %cf, align 1, !revenant.va !1917
  %o662 = xor i32 %t654, %165, !revenant.va !1917
  %n663 = xor i32 %o662, -1, !revenant.va !1917
  %o2664 = xor i32 %t654, %166, !revenant.va !1917
  %o3665 = and i32 %n663, %o2664, !revenant.va !1917
  %o4666 = and i32 %o3665, -2147483648, !revenant.va !1917
  %of667 = icmp ne i32 %o4666, 0, !revenant.va !1917
  store i1 %of667, ptr %of, align 1, !revenant.va !1917
  %z668 = zext i32 %166 to i64, !revenant.va !1917
  store i64 %z668, ptr %rax, align 8, !revenant.va !1917
  %r669 = load i64, ptr %rax, align 8, !revenant.va !1918
  %t670 = trunc i64 %r669 to i32, !revenant.va !1918
  %fp671 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1918
  %168 = ptrtoint ptr %fp671 to i64, !revenant.va !1918
  store i32 %t670, ptr %fp671, align 1, !revenant.va !1918
  %fp672 = getelementptr inbounds i8, ptr %sp0, i64 -24, !revenant.va !1919
  %169 = ptrtoint ptr %fp672 to i64, !revenant.va !1919
  store i64 %169, ptr %rdx, align 8, !revenant.va !1919
  %fp673 = getelementptr inbounds i8, ptr %sp0, i64 -24, !revenant.va !1920
  %170 = ptrtoint ptr %fp673 to i64, !revenant.va !1920
  store i64 %170, ptr %rcx, align 8, !revenant.va !1920
  %r674 = load i64, ptr %rcx, align 8, !revenant.va !1921
  %171 = add i64 %r674, 8, !revenant.va !1921
  %zf675 = icmp eq i64 %171, 0, !revenant.va !1921
  store i1 %zf675, ptr %zf, align 1, !revenant.va !1921
  %sf676 = and i64 %171, -9223372036854775808, !revenant.va !1921
  %sf677 = icmp ne i64 %sf676, 0, !revenant.va !1921
  store i1 %sf677, ptr %sf, align 1, !revenant.va !1921
  %l678 = trunc i64 %171 to i8, !revenant.va !1921
  %172 = call i8 @llvm.ctpop.i8(i8 %l678), !revenant.va !1921
  %pf679 = trunc i8 %172 to i1, !revenant.va !1921
  %pf680 = xor i1 %pf679, true, !revenant.va !1921
  store i1 %pf680, ptr %pf, align 1, !revenant.va !1921
  %cf681 = icmp ult i64 %171, %r674, !revenant.va !1921
  store i1 %cf681, ptr %cf, align 1, !revenant.va !1921
  %o682 = xor i64 %r674, 8, !revenant.va !1921
  %n683 = xor i64 %o682, -1, !revenant.va !1921
  %o2684 = xor i64 %r674, %171, !revenant.va !1921
  %o3685 = and i64 %n683, %o2684, !revenant.va !1921
  %o4686 = and i64 %o3685, -9223372036854775808, !revenant.va !1921
  %of687 = icmp ne i64 %o4686, 0, !revenant.va !1921
  store i1 %of687, ptr %of, align 1, !revenant.va !1921
  store i64 %171, ptr %rcx, align 8, !revenant.va !1921
  store i64 8, ptr %r8, align 8, !revenant.va !1922
  %r688 = load i64, ptr %rcx, align 8, !revenant.va !1923
  %r689 = load i64, ptr %rdx, align 8, !revenant.va !1923
  %r690 = load i64, ptr %r8, align 8, !revenant.va !1923
  %e691 = trunc i64 %r690 to i32, !revenant.va !1923
  %c692 = call i32 @sub_140001950(i64 %r688, i64 %r689, i32 %e691), !revenant.va !1923
  store i64 undef, ptr %rax, align 8, !revenant.va !1923
  store i64 undef, ptr %rcx, align 8, !revenant.va !1923
  store i64 undef, ptr %rdx, align 8, !revenant.va !1923
  store i1 undef, ptr %cf, align 1, !revenant.va !1923
  store i1 undef, ptr %of, align 1, !revenant.va !1923
  store i1 undef, ptr %pf, align 1, !revenant.va !1923
  store i1 undef, ptr %sf, align 1, !revenant.va !1923
  store i1 undef, ptr %zf, align 1, !revenant.va !1923
  %e693 = zext i32 %c692 to i64, !revenant.va !1923
  store i64 %e693, ptr %rax, align 8, !revenant.va !1923
  %fp694 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1924
  %173 = ptrtoint ptr %fp694 to i64, !revenant.va !1924
  %174 = load i32, ptr %fp694, align 1, !revenant.va !1924
  %r695 = load i64, ptr %rax, align 8, !revenant.va !1924
  %t696 = trunc i64 %r695 to i32, !revenant.va !1924
  %175 = add i32 %t696, %174, !revenant.va !1924
  %zf697 = icmp eq i32 %175, 0, !revenant.va !1924
  store i1 %zf697, ptr %zf, align 1, !revenant.va !1924
  %sf698 = and i32 %175, -2147483648, !revenant.va !1924
  %sf699 = icmp ne i32 %sf698, 0, !revenant.va !1924
  store i1 %sf699, ptr %sf, align 1, !revenant.va !1924
  %l700 = trunc i32 %175 to i8, !revenant.va !1924
  %176 = call i8 @llvm.ctpop.i8(i8 %l700), !revenant.va !1924
  %pf701 = trunc i8 %176 to i1, !revenant.va !1924
  %pf702 = xor i1 %pf701, true, !revenant.va !1924
  store i1 %pf702, ptr %pf, align 1, !revenant.va !1924
  %cf703 = icmp ult i32 %175, %t696, !revenant.va !1924
  store i1 %cf703, ptr %cf, align 1, !revenant.va !1924
  %o704 = xor i32 %t696, %174, !revenant.va !1924
  %n705 = xor i32 %o704, -1, !revenant.va !1924
  %o2706 = xor i32 %t696, %175, !revenant.va !1924
  %o3707 = and i32 %n705, %o2706, !revenant.va !1924
  %o4708 = and i32 %o3707, -2147483648, !revenant.va !1924
  %of709 = icmp ne i32 %o4708, 0, !revenant.va !1924
  store i1 %of709, ptr %of, align 1, !revenant.va !1924
  %z710 = zext i32 %175 to i64, !revenant.va !1924
  store i64 %z710, ptr %rax, align 8, !revenant.va !1924
  %r711 = load i64, ptr %rax, align 8, !revenant.va !1925
  %t712 = trunc i64 %r711 to i32, !revenant.va !1925
  %fp713 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1925
  %177 = ptrtoint ptr %fp713 to i64, !revenant.va !1925
  store i32 %t712, ptr %fp713, align 1, !revenant.va !1925
  %fp714 = getelementptr inbounds i8, ptr %sp0, i64 -21, !revenant.va !1926
  %178 = ptrtoint ptr %fp714 to i64, !revenant.va !1926
  %179 = load i8, ptr %fp714, align 1, !revenant.va !1926
  %180 = zext i8 %179 to i32, !revenant.va !1926
  %z715 = zext i32 %180 to i64, !revenant.va !1926
  store i64 %z715, ptr %rax, align 8, !revenant.va !1926
  %fp716 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1927
  %181 = ptrtoint ptr %fp716 to i64, !revenant.va !1927
  %182 = load i32, ptr %fp716, align 1, !revenant.va !1927
  %r717 = load i64, ptr %rax, align 8, !revenant.va !1927
  %t718 = trunc i64 %r717 to i32, !revenant.va !1927
  %183 = add i32 %t718, %182, !revenant.va !1927
  %zf719 = icmp eq i32 %183, 0, !revenant.va !1927
  store i1 %zf719, ptr %zf, align 1, !revenant.va !1927
  %sf720 = and i32 %183, -2147483648, !revenant.va !1927
  %sf721 = icmp ne i32 %sf720, 0, !revenant.va !1927
  store i1 %sf721, ptr %sf, align 1, !revenant.va !1927
  %l722 = trunc i32 %183 to i8, !revenant.va !1927
  %184 = call i8 @llvm.ctpop.i8(i8 %l722), !revenant.va !1927
  %pf723 = trunc i8 %184 to i1, !revenant.va !1927
  %pf724 = xor i1 %pf723, true, !revenant.va !1927
  store i1 %pf724, ptr %pf, align 1, !revenant.va !1927
  %cf725 = icmp ult i32 %183, %t718, !revenant.va !1927
  store i1 %cf725, ptr %cf, align 1, !revenant.va !1927
  %o726 = xor i32 %t718, %182, !revenant.va !1927
  %n727 = xor i32 %o726, -1, !revenant.va !1927
  %o2728 = xor i32 %t718, %183, !revenant.va !1927
  %o3729 = and i32 %n727, %o2728, !revenant.va !1927
  %o4730 = and i32 %o3729, -2147483648, !revenant.va !1927
  %of731 = icmp ne i32 %o4730, 0, !revenant.va !1927
  store i1 %of731, ptr %of, align 1, !revenant.va !1927
  %z732 = zext i32 %183 to i64, !revenant.va !1927
  store i64 %z732, ptr %rax, align 8, !revenant.va !1927
  %r733 = load i64, ptr %rax, align 8, !revenant.va !1928
  %t734 = trunc i64 %r733 to i32, !revenant.va !1928
  %fp735 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1928
  %185 = ptrtoint ptr %fp735 to i64, !revenant.va !1928
  store i32 %t734, ptr %fp735, align 1, !revenant.va !1928
  %fp736 = getelementptr inbounds i8, ptr %sp0, i64 -324, !revenant.va !1929
  %186 = ptrtoint ptr %fp736 to i64, !revenant.va !1929
  store i32 0, ptr %fp736, align 1, !revenant.va !1929
  br label %bb_140002048, !revenant.va !1929

bb_140002048:                                     ; preds = %bb_14000204f, %bb_140001df0
  %fp737 = getelementptr inbounds i8, ptr %sp0, i64 -324, !revenant.va !1930
  %187 = ptrtoint ptr %fp737 to i64, !revenant.va !1930
  %188 = load i32, ptr %fp737, align 1, !revenant.va !1930
  %cs = sub i32 %188, 16, !revenant.va !1930
  %zf738 = icmp eq i32 %cs, 0, !revenant.va !1930
  store i1 %zf738, ptr %zf, align 1, !revenant.va !1930
  %sf739 = and i32 %cs, -2147483648, !revenant.va !1930
  %sf740 = icmp ne i32 %sf739, 0, !revenant.va !1930
  store i1 %sf740, ptr %sf, align 1, !revenant.va !1930
  %l741 = trunc i32 %cs to i8, !revenant.va !1930
  %189 = call i8 @llvm.ctpop.i8(i8 %l741), !revenant.va !1930
  %pf742 = trunc i8 %189 to i1, !revenant.va !1930
  %pf743 = xor i1 %pf742, true, !revenant.va !1930
  store i1 %pf743, ptr %pf, align 1, !revenant.va !1930
  %cf744 = icmp ult i32 %188, 16, !revenant.va !1930
  store i1 %cf744, ptr %cf, align 1, !revenant.va !1930
  %o745 = xor i32 %188, 16, !revenant.va !1930
  %o2746 = xor i32 %188, %cs, !revenant.va !1930
  %o3747 = and i32 %o745, %o2746, !revenant.va !1930
  %o4748 = and i32 %o3747, -2147483648, !revenant.va !1930
  %of749 = icmp ne i32 %o4748, 0, !revenant.va !1930
  store i1 %of749, ptr %of, align 1, !revenant.va !1930
  %c750 = icmp sge i32 %188, 16, !revenant.va !1801
  br i1 %c750, label %bb_140002082, label %bb_14000204f, !revenant.va !1931

bb_14000204f:                                     ; preds = %bb_140002048
  %fp751 = getelementptr inbounds i8, ptr %sp0, i64 -324, !revenant.va !1932
  %190 = ptrtoint ptr %fp751 to i64, !revenant.va !1932
  %191 = load i32, ptr %fp751, align 1, !revenant.va !1932
  %z752 = zext i32 %191 to i64, !revenant.va !1932
  store i64 %z752, ptr %rcx, align 8, !revenant.va !1932
  %fp753 = getelementptr inbounds i8, ptr %sp0, i64 -316, !revenant.va !1933
  %192 = ptrtoint ptr %fp753 to i64, !revenant.va !1933
  %193 = load i32, ptr %fp753, align 1, !revenant.va !1933
  %r754 = load i64, ptr %rcx, align 8, !revenant.va !1933
  %t755 = trunc i64 %r754 to i32, !revenant.va !1933
  %194 = add i32 %t755, %193, !revenant.va !1933
  %zf756 = icmp eq i32 %194, 0, !revenant.va !1933
  store i1 %zf756, ptr %zf, align 1, !revenant.va !1933
  %sf757 = and i32 %194, -2147483648, !revenant.va !1933
  %sf758 = icmp ne i32 %sf757, 0, !revenant.va !1933
  store i1 %sf758, ptr %sf, align 1, !revenant.va !1933
  %l759 = trunc i32 %194 to i8, !revenant.va !1933
  %195 = call i8 @llvm.ctpop.i8(i8 %l759), !revenant.va !1933
  %pf760 = trunc i8 %195 to i1, !revenant.va !1933
  %pf761 = xor i1 %pf760, true, !revenant.va !1933
  store i1 %pf761, ptr %pf, align 1, !revenant.va !1933
  %cf762 = icmp ult i32 %194, %t755, !revenant.va !1933
  store i1 %cf762, ptr %cf, align 1, !revenant.va !1933
  %o763 = xor i32 %t755, %193, !revenant.va !1933
  %n764 = xor i32 %o763, -1, !revenant.va !1933
  %o2765 = xor i32 %t755, %194, !revenant.va !1933
  %o3766 = and i32 %n764, %o2765, !revenant.va !1933
  %o4767 = and i32 %o3766, -2147483648, !revenant.va !1933
  %of768 = icmp ne i32 %o4767, 0, !revenant.va !1933
  store i1 %of768, ptr %of, align 1, !revenant.va !1933
  %z769 = zext i32 %194 to i64, !revenant.va !1933
  store i64 %z769, ptr %rcx, align 8, !revenant.va !1933
  %fp770 = getelementptr inbounds i8, ptr %sp0, i64 -324, !revenant.va !1934
  %196 = ptrtoint ptr %fp770 to i64, !revenant.va !1934
  %197 = load i32, ptr %fp770, align 1, !revenant.va !1934
  %198 = sext i32 %197 to i64, !revenant.va !1934
  store i64 %198, ptr %rax, align 8, !revenant.va !1934
  %r771 = load i64, ptr %rcx, align 8, !revenant.va !1935
  %t772 = trunc i64 %r771 to i32, !revenant.va !1935
  %r773 = load i64, ptr %rax, align 8, !revenant.va !1935
  %sc = mul i64 %r773, 4, !revenant.va !1935
  %a = add i64 %sc, -88, !revenant.va !1935
  %fp774 = getelementptr inbounds i8, ptr %sp0, i64 %a, !revenant.va !1935
  %199 = ptrtoint ptr %fp774 to i64, !revenant.va !1935
  store i32 %t772, ptr %fp774, align 1, !revenant.va !1935
  %fp775 = getelementptr inbounds i8, ptr %sp0, i64 -324, !revenant.va !1936
  %200 = ptrtoint ptr %fp775 to i64, !revenant.va !1936
  %201 = load i32, ptr %fp775, align 1, !revenant.va !1936
  %z776 = zext i32 %201 to i64, !revenant.va !1936
  store i64 %z776, ptr %rcx, align 8, !revenant.va !1936
  %r777 = load i64, ptr %rcx, align 8, !revenant.va !1937
  %t778 = trunc i64 %r777 to i32, !revenant.va !1937
  %202 = shl i32 %t778, 1, !revenant.va !1937
  %203 = select i1 false, i32 0, i32 %202, !revenant.va !1937
  %204 = select i1 false, i32 %t778, i32 %203, !revenant.va !1937
  %205 = icmp eq i32 %204, 0, !revenant.va !1937
  %zf779 = load i1, ptr %zf, align 1, !revenant.va !1937
  %206 = select i1 false, i1 %zf779, i1 %205, !revenant.va !1937
  store i1 %206, ptr %zf, align 1, !revenant.va !1937
  %207 = icmp slt i32 %204, 0, !revenant.va !1937
  %sf780 = load i1, ptr %sf, align 1, !revenant.va !1937
  %208 = select i1 false, i1 %sf780, i1 %207, !revenant.va !1937
  store i1 %208, ptr %sf, align 1, !revenant.va !1937
  %209 = trunc i32 %204 to i8, !revenant.va !1937
  %210 = call i8 @llvm.ctpop.i8(i8 %209), !revenant.va !1937
  %211 = trunc i8 %210 to i1, !revenant.va !1937
  %212 = xor i1 %211, true, !revenant.va !1937
  %pf781 = load i1, ptr %pf, align 1, !revenant.va !1937
  %213 = select i1 false, i1 %pf781, i1 %212, !revenant.va !1937
  store i1 %213, ptr %pf, align 1, !revenant.va !1937
  %214 = zext i32 %t778 to i64, !revenant.va !1937
  %215 = lshr i64 %214, 31, !revenant.va !1937
  %216 = trunc i64 %215 to i1, !revenant.va !1937
  %217 = select i1 true, i1 %216, i1 undef, !revenant.va !1937
  %cf782 = load i1, ptr %cf, align 1, !revenant.va !1937
  %218 = select i1 false, i1 %cf782, i1 %217, !revenant.va !1937
  store i1 %218, ptr %cf, align 1, !revenant.va !1937
  %219 = and i32 %204, -2147483648, !revenant.va !1937
  %220 = icmp ne i32 %219, 0, !revenant.va !1937
  %221 = xor i1 %220, %216, !revenant.va !1937
  %222 = select i1 true, i1 %221, i1 undef, !revenant.va !1937
  %of783 = load i1, ptr %of, align 1, !revenant.va !1937
  %223 = select i1 false, i1 %of783, i1 %222, !revenant.va !1937
  store i1 %223, ptr %of, align 1, !revenant.va !1937
  %z784 = zext i32 %204 to i64, !revenant.va !1937
  store i64 %z784, ptr %rcx, align 8, !revenant.va !1937
  %fp785 = getelementptr inbounds i8, ptr %sp0, i64 -324, !revenant.va !1938
  %224 = ptrtoint ptr %fp785 to i64, !revenant.va !1938
  %225 = load i32, ptr %fp785, align 1, !revenant.va !1938
  %226 = sext i32 %225 to i64, !revenant.va !1938
  store i64 %226, ptr %rax, align 8, !revenant.va !1938
  %r786 = load i64, ptr %rcx, align 8, !revenant.va !1939
  %t787 = trunc i64 %r786 to i32, !revenant.va !1939
  %r788 = load i64, ptr %rax, align 8, !revenant.va !1939
  %sc789 = mul i64 %r788, 4, !revenant.va !1939
  %a790 = add i64 %sc789, -152, !revenant.va !1939
  %fp791 = getelementptr inbounds i8, ptr %sp0, i64 %a790, !revenant.va !1939
  %227 = ptrtoint ptr %fp791 to i64, !revenant.va !1939
  store i32 %t787, ptr %fp791, align 1, !revenant.va !1939
  %fp792 = getelementptr inbounds i8, ptr %sp0, i64 -324, !revenant.va !1940
  %228 = ptrtoint ptr %fp792 to i64, !revenant.va !1940
  %229 = load i32, ptr %fp792, align 1, !revenant.va !1940
  %z793 = zext i32 %229 to i64, !revenant.va !1940
  store i64 %z793, ptr %rax, align 8, !revenant.va !1940
  %r794 = load i64, ptr %rax, align 8, !revenant.va !1941
  %t795 = trunc i64 %r794 to i32, !revenant.va !1941
  %230 = add i32 %t795, 1, !revenant.va !1941
  %zf796 = icmp eq i32 %230, 0, !revenant.va !1941
  store i1 %zf796, ptr %zf, align 1, !revenant.va !1941
  %sf797 = and i32 %230, -2147483648, !revenant.va !1941
  %sf798 = icmp ne i32 %sf797, 0, !revenant.va !1941
  store i1 %sf798, ptr %sf, align 1, !revenant.va !1941
  %l799 = trunc i32 %230 to i8, !revenant.va !1941
  %231 = call i8 @llvm.ctpop.i8(i8 %l799), !revenant.va !1941
  %pf800 = trunc i8 %231 to i1, !revenant.va !1941
  %pf801 = xor i1 %pf800, true, !revenant.va !1941
  store i1 %pf801, ptr %pf, align 1, !revenant.va !1941
  %cf802 = icmp ult i32 %230, %t795, !revenant.va !1941
  store i1 %cf802, ptr %cf, align 1, !revenant.va !1941
  %o803 = xor i32 %t795, 1, !revenant.va !1941
  %n804 = xor i32 %o803, -1, !revenant.va !1941
  %o2805 = xor i32 %t795, %230, !revenant.va !1941
  %o3806 = and i32 %n804, %o2805, !revenant.va !1941
  %o4807 = and i32 %o3806, -2147483648, !revenant.va !1941
  %of808 = icmp ne i32 %o4807, 0, !revenant.va !1941
  store i1 %of808, ptr %of, align 1, !revenant.va !1941
  %z809 = zext i32 %230 to i64, !revenant.va !1941
  store i64 %z809, ptr %rax, align 8, !revenant.va !1941
  %r810 = load i64, ptr %rax, align 8, !revenant.va !1942
  %t811 = trunc i64 %r810 to i32, !revenant.va !1942
  %fp812 = getelementptr inbounds i8, ptr %sp0, i64 -324, !revenant.va !1942
  %232 = ptrtoint ptr %fp812 to i64, !revenant.va !1942
  store i32 %t811, ptr %fp812, align 1, !revenant.va !1942
  br label %bb_140002048, !revenant.va !1943

bb_140002082:                                     ; preds = %bb_140002048
  %fp813 = getelementptr inbounds i8, ptr %sp0, i64 -328, !revenant.va !1944
  %233 = ptrtoint ptr %fp813 to i64, !revenant.va !1944
  store i32 0, ptr %fp813, align 1, !revenant.va !1944
  br label %bb_14000208a, !revenant.va !1944

bb_14000208a:                                     ; preds = %bb_140002091, %bb_140002082
  %fp814 = getelementptr inbounds i8, ptr %sp0, i64 -328, !revenant.va !1945
  %234 = ptrtoint ptr %fp814 to i64, !revenant.va !1945
  %235 = load i32, ptr %fp814, align 1, !revenant.va !1945
  %cs815 = sub i32 %235, 8, !revenant.va !1945
  %zf816 = icmp eq i32 %cs815, 0, !revenant.va !1945
  store i1 %zf816, ptr %zf, align 1, !revenant.va !1945
  %sf817 = and i32 %cs815, -2147483648, !revenant.va !1945
  %sf818 = icmp ne i32 %sf817, 0, !revenant.va !1945
  store i1 %sf818, ptr %sf, align 1, !revenant.va !1945
  %l819 = trunc i32 %cs815 to i8, !revenant.va !1945
  %236 = call i8 @llvm.ctpop.i8(i8 %l819), !revenant.va !1945
  %pf820 = trunc i8 %236 to i1, !revenant.va !1945
  %pf821 = xor i1 %pf820, true, !revenant.va !1945
  store i1 %pf821, ptr %pf, align 1, !revenant.va !1945
  %cf822 = icmp ult i32 %235, 8, !revenant.va !1945
  store i1 %cf822, ptr %cf, align 1, !revenant.va !1945
  %o823 = xor i32 %235, 8, !revenant.va !1945
  %o2824 = xor i32 %235, %cs815, !revenant.va !1945
  %o3825 = and i32 %o823, %o2824, !revenant.va !1945
  %o4826 = and i32 %o3825, -2147483648, !revenant.va !1945
  %of827 = icmp ne i32 %o4826, 0, !revenant.va !1945
  store i1 %of827, ptr %of, align 1, !revenant.va !1945
  %c828 = icmp sge i32 %235, 8, !revenant.va !1801
  br i1 %c828, label %bb_1400020bb, label %bb_140002091, !revenant.va !1946

bb_140002091:                                     ; preds = %bb_14000208a
  %fp829 = getelementptr inbounds i8, ptr %sp0, i64 -328, !revenant.va !1947
  %237 = ptrtoint ptr %fp829 to i64, !revenant.va !1947
  %238 = load i32, ptr %fp829, align 1, !revenant.va !1947
  %239 = zext i32 %238 to i128, !revenant.va !1947
  %240 = sitofp i32 %238 to double, !revenant.va !1947
  %x = load i128, ptr %xmm0, align 16, !revenant.va !1947
  %241 = bitcast i128 %x to <2 x double>, !revenant.va !1947
  %242 = insertelement <2 x double> %241, double %240, i64 0, !revenant.va !1947
  %243 = bitcast <2 x double> %242 to i128, !revenant.va !1947
  store i128 %243, ptr %xmm0, align 16, !revenant.va !1947
  %244 = load double, ptr @g_1400710d0, align 1, !revenant.va !1948
  %245 = insertelement <2 x double> zeroinitializer, double %244, i64 0, !revenant.va !1948
  %246 = bitcast <2 x double> %245 to i128, !revenant.va !1948
  store i128 %246, ptr %xmm1, align 16, !revenant.va !1948
  %x830 = load i128, ptr %xmm1, align 16, !revenant.va !1949
  %247 = trunc i128 %x830 to i64, !revenant.va !1949
  %248 = zext i64 %247 to i128, !revenant.va !1949
  %249 = bitcast i128 %248 to <2 x double>, !revenant.va !1949
  %250 = extractelement <2 x double> %249, i64 0, !revenant.va !1949
  %x831 = load i128, ptr %xmm0, align 16, !revenant.va !1949
  %251 = trunc i128 %x831 to i64, !revenant.va !1949
  %252 = zext i64 %251 to i128, !revenant.va !1949
  %253 = bitcast i128 %252 to <2 x double>, !revenant.va !1949
  %254 = extractelement <2 x double> %253, i64 0, !revenant.va !1949
  %255 = fadd double %254, %250, !revenant.va !1949
  %x832 = load i128, ptr %xmm0, align 16, !revenant.va !1949
  %256 = bitcast i128 %x832 to <2 x double>, !revenant.va !1949
  %257 = insertelement <2 x double> %256, double %255, i64 0, !revenant.va !1949
  %258 = bitcast <2 x double> %257 to i128, !revenant.va !1949
  store i128 %258, ptr %xmm0, align 16, !revenant.va !1949
  %fp833 = getelementptr inbounds i8, ptr %sp0, i64 -328, !revenant.va !1950
  %259 = ptrtoint ptr %fp833 to i64, !revenant.va !1950
  %260 = load i32, ptr %fp833, align 1, !revenant.va !1950
  %261 = sext i32 %260 to i64, !revenant.va !1950
  store i64 %261, ptr %rax, align 8, !revenant.va !1950
  %x834 = load i128, ptr %xmm0, align 16, !revenant.va !1951
  %262 = trunc i128 %x834 to i64, !revenant.va !1951
  %263 = zext i64 %262 to i128, !revenant.va !1951
  %264 = bitcast i128 %263 to <2 x double>, !revenant.va !1951
  %265 = extractelement <2 x double> %264, i64 0, !revenant.va !1951
  %r835 = load i64, ptr %rax, align 8, !revenant.va !1951
  %sc836 = mul i64 %r835, 8, !revenant.va !1951
  %a837 = add i64 %sc836, -280, !revenant.va !1951
  %fp838 = getelementptr inbounds i8, ptr %sp0, i64 %a837, !revenant.va !1951
  %266 = ptrtoint ptr %fp838 to i64, !revenant.va !1951
  store double %265, ptr %fp838, align 1, !revenant.va !1951
  %fp839 = getelementptr inbounds i8, ptr %sp0, i64 -328, !revenant.va !1952
  %267 = ptrtoint ptr %fp839 to i64, !revenant.va !1952
  %268 = load i32, ptr %fp839, align 1, !revenant.va !1952
  %z840 = zext i32 %268 to i64, !revenant.va !1952
  store i64 %z840, ptr %rax, align 8, !revenant.va !1952
  %r841 = load i64, ptr %rax, align 8, !revenant.va !1953
  %t842 = trunc i64 %r841 to i32, !revenant.va !1953
  %269 = add i32 %t842, 1, !revenant.va !1953
  %zf843 = icmp eq i32 %269, 0, !revenant.va !1953
  store i1 %zf843, ptr %zf, align 1, !revenant.va !1953
  %sf844 = and i32 %269, -2147483648, !revenant.va !1953
  %sf845 = icmp ne i32 %sf844, 0, !revenant.va !1953
  store i1 %sf845, ptr %sf, align 1, !revenant.va !1953
  %l846 = trunc i32 %269 to i8, !revenant.va !1953
  %270 = call i8 @llvm.ctpop.i8(i8 %l846), !revenant.va !1953
  %pf847 = trunc i8 %270 to i1, !revenant.va !1953
  %pf848 = xor i1 %pf847, true, !revenant.va !1953
  store i1 %pf848, ptr %pf, align 1, !revenant.va !1953
  %cf849 = icmp ult i32 %269, %t842, !revenant.va !1953
  store i1 %cf849, ptr %cf, align 1, !revenant.va !1953
  %o850 = xor i32 %t842, 1, !revenant.va !1953
  %n851 = xor i32 %o850, -1, !revenant.va !1953
  %o2852 = xor i32 %t842, %269, !revenant.va !1953
  %o3853 = and i32 %n851, %o2852, !revenant.va !1953
  %o4854 = and i32 %o3853, -2147483648, !revenant.va !1953
  %of855 = icmp ne i32 %o4854, 0, !revenant.va !1953
  store i1 %of855, ptr %of, align 1, !revenant.va !1953
  %z856 = zext i32 %269 to i64, !revenant.va !1953
  store i64 %z856, ptr %rax, align 8, !revenant.va !1953
  %r857 = load i64, ptr %rax, align 8, !revenant.va !1954
  %t858 = trunc i64 %r857 to i32, !revenant.va !1954
  %fp859 = getelementptr inbounds i8, ptr %sp0, i64 -328, !revenant.va !1954
  %271 = ptrtoint ptr %fp859 to i64, !revenant.va !1954
  store i32 %t858, ptr %fp859, align 1, !revenant.va !1954
  br label %bb_14000208a, !revenant.va !1955

bb_1400020bb:                                     ; preds = %bb_14000208a
  %fp860 = getelementptr inbounds i8, ptr %sp0, i64 -88, !revenant.va !1956
  %272 = ptrtoint ptr %fp860 to i64, !revenant.va !1956
  store i64 %272, ptr %rcx, align 8, !revenant.va !1956
  store i64 16, ptr %rdx, align 8, !revenant.va !1957
  %r861 = load i64, ptr %rcx, align 8, !revenant.va !1958
  %r862 = load i64, ptr %rdx, align 8, !revenant.va !1958
  %e863 = trunc i64 %r862 to i32, !revenant.va !1958
  %c864 = call float @sub_140001A30(i64 %r861, i32 %e863), !revenant.va !1958
  store i64 undef, ptr %rax, align 8, !revenant.va !1958
  store i64 undef, ptr %rcx, align 8, !revenant.va !1958
  store i1 undef, ptr %cf, align 1, !revenant.va !1958
  store i1 undef, ptr %of, align 1, !revenant.va !1958
  store i1 undef, ptr %pf, align 1, !revenant.va !1958
  store i1 undef, ptr %sf, align 1, !revenant.va !1958
  store i1 undef, ptr %zf, align 1, !revenant.va !1958
  %cr = bitcast float %c864 to i32, !revenant.va !1958
  %e865 = zext i32 %cr to i128, !revenant.va !1958
  store i128 %e865, ptr %xmm0, align 16, !revenant.va !1958
  %fp866 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1959
  %273 = ptrtoint ptr %fp866 to i64, !revenant.va !1959
  %274 = load i32, ptr %fp866, align 1, !revenant.va !1959
  %r867 = load i64, ptr %rax, align 8, !revenant.va !1959
  %t868 = trunc i64 %r867 to i32, !revenant.va !1959
  %275 = add i32 %t868, %274, !revenant.va !1959
  %zf869 = icmp eq i32 %275, 0, !revenant.va !1959
  store i1 %zf869, ptr %zf, align 1, !revenant.va !1959
  %sf870 = and i32 %275, -2147483648, !revenant.va !1959
  %sf871 = icmp ne i32 %sf870, 0, !revenant.va !1959
  store i1 %sf871, ptr %sf, align 1, !revenant.va !1959
  %l872 = trunc i32 %275 to i8, !revenant.va !1959
  %276 = call i8 @llvm.ctpop.i8(i8 %l872), !revenant.va !1959
  %pf873 = trunc i8 %276 to i1, !revenant.va !1959
  %pf874 = xor i1 %pf873, true, !revenant.va !1959
  store i1 %pf874, ptr %pf, align 1, !revenant.va !1959
  %cf875 = icmp ult i32 %275, %t868, !revenant.va !1959
  store i1 %cf875, ptr %cf, align 1, !revenant.va !1959
  %o876 = xor i32 %t868, %274, !revenant.va !1959
  %n877 = xor i32 %o876, -1, !revenant.va !1959
  %o2878 = xor i32 %t868, %275, !revenant.va !1959
  %o3879 = and i32 %n877, %o2878, !revenant.va !1959
  %o4880 = and i32 %o3879, -2147483648, !revenant.va !1959
  %of881 = icmp ne i32 %o4880, 0, !revenant.va !1959
  store i1 %of881, ptr %of, align 1, !revenant.va !1959
  %z882 = zext i32 %275 to i64, !revenant.va !1959
  store i64 %z882, ptr %rax, align 8, !revenant.va !1959
  %r883 = load i64, ptr %rax, align 8, !revenant.va !1960
  %t884 = trunc i64 %r883 to i32, !revenant.va !1960
  %fp885 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1960
  %277 = ptrtoint ptr %fp885 to i64, !revenant.va !1960
  store i32 %t884, ptr %fp885, align 1, !revenant.va !1960
  %fp886 = getelementptr inbounds i8, ptr %sp0, i64 -88, !revenant.va !1961
  %278 = ptrtoint ptr %fp886 to i64, !revenant.va !1961
  store i64 %278, ptr %rdx, align 8, !revenant.va !1961
  %fp887 = getelementptr inbounds i8, ptr %sp0, i64 -216, !revenant.va !1962
  %279 = ptrtoint ptr %fp887 to i64, !revenant.va !1962
  store i64 %279, ptr %rcx, align 8, !revenant.va !1962
  store i64 16, ptr %r8, align 8, !revenant.va !1963
  %r888 = load i64, ptr %rcx, align 8, !revenant.va !1964
  %r889 = load i64, ptr %rdx, align 8, !revenant.va !1964
  %r890 = load i64, ptr %r8, align 8, !revenant.va !1964
  %e891 = trunc i64 %r890 to i32, !revenant.va !1964
  %c892 = call float @sub_140001A80(i64 %r888, i64 %r889, i32 %e891), !revenant.va !1964
  store i64 undef, ptr %rax, align 8, !revenant.va !1964
  store i64 undef, ptr %rcx, align 8, !revenant.va !1964
  store i64 undef, ptr %rdx, align 8, !revenant.va !1964
  store i1 undef, ptr %cf, align 1, !revenant.va !1964
  store i1 undef, ptr %of, align 1, !revenant.va !1964
  store i1 undef, ptr %pf, align 1, !revenant.va !1964
  store i1 undef, ptr %sf, align 1, !revenant.va !1964
  store i1 undef, ptr %zf, align 1, !revenant.va !1964
  %cr893 = bitcast float %c892 to i32, !revenant.va !1964
  %e894 = zext i32 %cr893 to i128, !revenant.va !1964
  store i128 %e894, ptr %xmm0, align 16, !revenant.va !1964
  %fp895 = getelementptr inbounds i8, ptr %sp0, i64 -216, !revenant.va !1965
  %280 = ptrtoint ptr %fp895 to i64, !revenant.va !1965
  store i64 %280, ptr %rcx, align 8, !revenant.va !1965
  store i64 3, ptr %rdx, align 8, !revenant.va !1966
  store i64 16, ptr %r8, align 8, !revenant.va !1967
  %r896 = load i64, ptr %rcx, align 8, !revenant.va !1968
  %r897 = load i64, ptr %rdx, align 8, !revenant.va !1968
  %e898 = trunc i64 %r897 to i32, !revenant.va !1968
  %r899 = load i64, ptr %r8, align 8, !revenant.va !1968
  %e900 = trunc i64 %r899 to i32, !revenant.va !1968
  %c901 = call float @sub_140001AE0(i64 %r896, i32 %e898, i32 %e900), !revenant.va !1968
  store i64 undef, ptr %rax, align 8, !revenant.va !1968
  store i64 undef, ptr %rcx, align 8, !revenant.va !1968
  store i64 undef, ptr %rdx, align 8, !revenant.va !1968
  store i1 undef, ptr %cf, align 1, !revenant.va !1968
  store i1 undef, ptr %of, align 1, !revenant.va !1968
  store i1 undef, ptr %pf, align 1, !revenant.va !1968
  store i1 undef, ptr %sf, align 1, !revenant.va !1968
  store i1 undef, ptr %zf, align 1, !revenant.va !1968
  %cr902 = bitcast float %c901 to i32, !revenant.va !1968
  %e903 = zext i32 %cr902 to i128, !revenant.va !1968
  store i128 %e903, ptr %xmm0, align 16, !revenant.va !1968
  %fp904 = getelementptr inbounds i8, ptr %sp0, i64 -88, !revenant.va !1969
  %281 = ptrtoint ptr %fp904 to i64, !revenant.va !1969
  store i64 %281, ptr %rdx, align 8, !revenant.va !1969
  %fp905 = getelementptr inbounds i8, ptr %sp0, i64 -216, !revenant.va !1970
  %282 = ptrtoint ptr %fp905 to i64, !revenant.va !1970
  store i64 %282, ptr %rcx, align 8, !revenant.va !1970
  store i64 5, ptr %r8, align 8, !revenant.va !1971
  store i64 16, ptr %r9, align 8, !revenant.va !1972
  %r906 = load i64, ptr %rcx, align 8, !revenant.va !1973
  %r907 = load i64, ptr %rdx, align 8, !revenant.va !1973
  %r908 = load i64, ptr %r8, align 8, !revenant.va !1973
  %e909 = trunc i64 %r908 to i32, !revenant.va !1973
  %r910 = load i64, ptr %r9, align 8, !revenant.va !1973
  %e911 = trunc i64 %r910 to i32, !revenant.va !1973
  %c912 = call float @sub_140001B30(i64 %r906, i64 %r907, i32 %e909, i32 %e911), !revenant.va !1973
  store i64 undef, ptr %rax, align 8, !revenant.va !1973
  store i64 undef, ptr %rcx, align 8, !revenant.va !1973
  store i64 undef, ptr %rdx, align 8, !revenant.va !1973
  store i1 undef, ptr %cf, align 1, !revenant.va !1973
  store i1 undef, ptr %of, align 1, !revenant.va !1973
  store i1 undef, ptr %pf, align 1, !revenant.va !1973
  store i1 undef, ptr %sf, align 1, !revenant.va !1973
  store i1 undef, ptr %zf, align 1, !revenant.va !1973
  %cr913 = bitcast float %c912 to i32, !revenant.va !1973
  %e914 = zext i32 %cr913 to i128, !revenant.va !1973
  store i128 %e914, ptr %xmm0, align 16, !revenant.va !1973
  %fp915 = getelementptr inbounds i8, ptr %sp0, i64 -280, !revenant.va !1974
  %283 = ptrtoint ptr %fp915 to i64, !revenant.va !1974
  store i64 %283, ptr %rcx, align 8, !revenant.va !1974
  store i64 8, ptr %rdx, align 8, !revenant.va !1975
  %r916 = load i64, ptr %rcx, align 8, !revenant.va !1976
  %r917 = load i64, ptr %rdx, align 8, !revenant.va !1976
  %e918 = trunc i64 %r917 to i32, !revenant.va !1976
  %c919 = call double @sub_140001B90(i64 %r916, i32 %e918), !revenant.va !1976
  store i64 undef, ptr %rax, align 8, !revenant.va !1976
  store i64 undef, ptr %rcx, align 8, !revenant.va !1976
  store i128 undef, ptr %xmm0, align 16, !revenant.va !1976
  store i1 undef, ptr %cf, align 1, !revenant.va !1976
  store i1 undef, ptr %of, align 1, !revenant.va !1976
  store i1 undef, ptr %pf, align 1, !revenant.va !1976
  store i1 undef, ptr %sf, align 1, !revenant.va !1976
  store i1 undef, ptr %zf, align 1, !revenant.va !1976
  %cr920 = bitcast double %c919 to i64, !revenant.va !1976
  %e921 = zext i64 %cr920 to i128, !revenant.va !1976
  store i128 %e921, ptr %xmm0, align 16, !revenant.va !1976
  %x922 = load i128, ptr %xmm0, align 16, !revenant.va !1977
  %284 = trunc i128 %x922 to i64, !revenant.va !1977
  %285 = zext i64 %284 to i128, !revenant.va !1977
  %286 = bitcast i128 %285 to <2 x double>, !revenant.va !1977
  %287 = extractelement <2 x double> %286, i64 0, !revenant.va !1977
  %288 = fptosi double %287 to i32, !revenant.va !1977
  %z923 = zext i32 %288 to i64, !revenant.va !1977
  store i64 %z923, ptr %rax, align 8, !revenant.va !1977
  %fp924 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1978
  %289 = ptrtoint ptr %fp924 to i64, !revenant.va !1978
  %290 = load i32, ptr %fp924, align 1, !revenant.va !1978
  %r925 = load i64, ptr %rax, align 8, !revenant.va !1978
  %t926 = trunc i64 %r925 to i32, !revenant.va !1978
  %291 = add i32 %t926, %290, !revenant.va !1978
  %zf927 = icmp eq i32 %291, 0, !revenant.va !1978
  store i1 %zf927, ptr %zf, align 1, !revenant.va !1978
  %sf928 = and i32 %291, -2147483648, !revenant.va !1978
  %sf929 = icmp ne i32 %sf928, 0, !revenant.va !1978
  store i1 %sf929, ptr %sf, align 1, !revenant.va !1978
  %l930 = trunc i32 %291 to i8, !revenant.va !1978
  %292 = call i8 @llvm.ctpop.i8(i8 %l930), !revenant.va !1978
  %pf931 = trunc i8 %292 to i1, !revenant.va !1978
  %pf932 = xor i1 %pf931, true, !revenant.va !1978
  store i1 %pf932, ptr %pf, align 1, !revenant.va !1978
  %cf933 = icmp ult i32 %291, %t926, !revenant.va !1978
  store i1 %cf933, ptr %cf, align 1, !revenant.va !1978
  %o934 = xor i32 %t926, %290, !revenant.va !1978
  %n935 = xor i32 %o934, -1, !revenant.va !1978
  %o2936 = xor i32 %t926, %291, !revenant.va !1978
  %o3937 = and i32 %n935, %o2936, !revenant.va !1978
  %o4938 = and i32 %o3937, -2147483648, !revenant.va !1978
  %of939 = icmp ne i32 %o4938, 0, !revenant.va !1978
  store i1 %of939, ptr %of, align 1, !revenant.va !1978
  %z940 = zext i32 %291 to i64, !revenant.va !1978
  store i64 %z940, ptr %rax, align 8, !revenant.va !1978
  %r941 = load i64, ptr %rax, align 8, !revenant.va !1979
  %t942 = trunc i64 %r941 to i32, !revenant.va !1979
  %fp943 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1979
  %293 = ptrtoint ptr %fp943 to i64, !revenant.va !1979
  store i32 %t942, ptr %fp943, align 1, !revenant.va !1979
  %fp944 = getelementptr inbounds i8, ptr %sp0, i64 -152, !revenant.va !1980
  %294 = ptrtoint ptr %fp944 to i64, !revenant.va !1980
  store i64 %294, ptr %rdx, align 8, !revenant.va !1980
  %fp945 = getelementptr inbounds i8, ptr %sp0, i64 -88, !revenant.va !1981
  %295 = ptrtoint ptr %fp945 to i64, !revenant.va !1981
  store i64 %295, ptr %rcx, align 8, !revenant.va !1981
  store i64 16, ptr %r8, align 8, !revenant.va !1982
  %r946 = load i64, ptr %rcx, align 8, !revenant.va !1983
  %r947 = load i64, ptr %rdx, align 8, !revenant.va !1983
  %r948 = load i64, ptr %r8, align 8, !revenant.va !1983
  %e949 = trunc i64 %r948 to i32, !revenant.va !1983
  %c950 = call i32 @sub_140001BF0(i64 %r946, i64 %r947, i32 %e949), !revenant.va !1983
  store i64 undef, ptr %rax, align 8, !revenant.va !1983
  store i64 undef, ptr %rcx, align 8, !revenant.va !1983
  store i64 undef, ptr %rdx, align 8, !revenant.va !1983
  store i1 undef, ptr %cf, align 1, !revenant.va !1983
  store i1 undef, ptr %of, align 1, !revenant.va !1983
  store i1 undef, ptr %pf, align 1, !revenant.va !1983
  store i1 undef, ptr %sf, align 1, !revenant.va !1983
  store i1 undef, ptr %zf, align 1, !revenant.va !1983
  %e951 = zext i32 %c950 to i64, !revenant.va !1983
  store i64 %e951, ptr %rax, align 8, !revenant.va !1983
  %fp952 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1984
  %296 = ptrtoint ptr %fp952 to i64, !revenant.va !1984
  %297 = load i32, ptr %fp952, align 1, !revenant.va !1984
  %r953 = load i64, ptr %rax, align 8, !revenant.va !1984
  %t954 = trunc i64 %r953 to i32, !revenant.va !1984
  %298 = add i32 %t954, %297, !revenant.va !1984
  %zf955 = icmp eq i32 %298, 0, !revenant.va !1984
  store i1 %zf955, ptr %zf, align 1, !revenant.va !1984
  %sf956 = and i32 %298, -2147483648, !revenant.va !1984
  %sf957 = icmp ne i32 %sf956, 0, !revenant.va !1984
  store i1 %sf957, ptr %sf, align 1, !revenant.va !1984
  %l958 = trunc i32 %298 to i8, !revenant.va !1984
  %299 = call i8 @llvm.ctpop.i8(i8 %l958), !revenant.va !1984
  %pf959 = trunc i8 %299 to i1, !revenant.va !1984
  %pf960 = xor i1 %pf959, true, !revenant.va !1984
  store i1 %pf960, ptr %pf, align 1, !revenant.va !1984
  %cf961 = icmp ult i32 %298, %t954, !revenant.va !1984
  store i1 %cf961, ptr %cf, align 1, !revenant.va !1984
  %o962 = xor i32 %t954, %297, !revenant.va !1984
  %n963 = xor i32 %o962, -1, !revenant.va !1984
  %o2964 = xor i32 %t954, %298, !revenant.va !1984
  %o3965 = and i32 %n963, %o2964, !revenant.va !1984
  %o4966 = and i32 %o3965, -2147483648, !revenant.va !1984
  %of967 = icmp ne i32 %o4966, 0, !revenant.va !1984
  store i1 %of967, ptr %of, align 1, !revenant.va !1984
  %z968 = zext i32 %298 to i64, !revenant.va !1984
  store i64 %z968, ptr %rax, align 8, !revenant.va !1984
  %r969 = load i64, ptr %rax, align 8, !revenant.va !1985
  %t970 = trunc i64 %r969 to i32, !revenant.va !1985
  %fp971 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1985
  %300 = ptrtoint ptr %fp971 to i64, !revenant.va !1985
  store i32 %t970, ptr %fp971, align 1, !revenant.va !1985
  %fp972 = getelementptr inbounds i8, ptr %sp0, i64 -88, !revenant.va !1986
  %301 = ptrtoint ptr %fp972 to i64, !revenant.va !1986
  store i64 %301, ptr %rcx, align 8, !revenant.va !1986
  store i64 16, ptr %rdx, align 8, !revenant.va !1987
  %r973 = load i64, ptr %rcx, align 8, !revenant.va !1988
  %r974 = load i64, ptr %rdx, align 8, !revenant.va !1988
  %e975 = trunc i64 %r974 to i32, !revenant.va !1988
  %c976 = call i32 @sub_140001C60(i64 %r973, i32 %e975), !revenant.va !1988
  store i64 undef, ptr %rax, align 8, !revenant.va !1988
  store i64 undef, ptr %rcx, align 8, !revenant.va !1988
  store i64 undef, ptr %rdx, align 8, !revenant.va !1988
  store i1 undef, ptr %cf, align 1, !revenant.va !1988
  store i1 undef, ptr %of, align 1, !revenant.va !1988
  store i1 undef, ptr %pf, align 1, !revenant.va !1988
  store i1 undef, ptr %sf, align 1, !revenant.va !1988
  store i1 undef, ptr %zf, align 1, !revenant.va !1988
  %e977 = zext i32 %c976 to i64, !revenant.va !1988
  store i64 %e977, ptr %rax, align 8, !revenant.va !1988
  %fp978 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1989
  %302 = ptrtoint ptr %fp978 to i64, !revenant.va !1989
  %303 = load i32, ptr %fp978, align 1, !revenant.va !1989
  %r979 = load i64, ptr %rax, align 8, !revenant.va !1989
  %t980 = trunc i64 %r979 to i32, !revenant.va !1989
  %304 = add i32 %t980, %303, !revenant.va !1989
  %zf981 = icmp eq i32 %304, 0, !revenant.va !1989
  store i1 %zf981, ptr %zf, align 1, !revenant.va !1989
  %sf982 = and i32 %304, -2147483648, !revenant.va !1989
  %sf983 = icmp ne i32 %sf982, 0, !revenant.va !1989
  store i1 %sf983, ptr %sf, align 1, !revenant.va !1989
  %l984 = trunc i32 %304 to i8, !revenant.va !1989
  %305 = call i8 @llvm.ctpop.i8(i8 %l984), !revenant.va !1989
  %pf985 = trunc i8 %305 to i1, !revenant.va !1989
  %pf986 = xor i1 %pf985, true, !revenant.va !1989
  store i1 %pf986, ptr %pf, align 1, !revenant.va !1989
  %cf987 = icmp ult i32 %304, %t980, !revenant.va !1989
  store i1 %cf987, ptr %cf, align 1, !revenant.va !1989
  %o988 = xor i32 %t980, %303, !revenant.va !1989
  %n989 = xor i32 %o988, -1, !revenant.va !1989
  %o2990 = xor i32 %t980, %304, !revenant.va !1989
  %o3991 = and i32 %n989, %o2990, !revenant.va !1989
  %o4992 = and i32 %o3991, -2147483648, !revenant.va !1989
  %of993 = icmp ne i32 %o4992, 0, !revenant.va !1989
  store i1 %of993, ptr %of, align 1, !revenant.va !1989
  %z994 = zext i32 %304 to i64, !revenant.va !1989
  store i64 %z994, ptr %rax, align 8, !revenant.va !1989
  %r995 = load i64, ptr %rax, align 8, !revenant.va !1990
  %t996 = trunc i64 %r995 to i32, !revenant.va !1990
  %fp997 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1990
  %306 = ptrtoint ptr %fp997 to i64, !revenant.va !1990
  store i32 %t996, ptr %fp997, align 1, !revenant.va !1990
  %fp998 = getelementptr inbounds i8, ptr %sp0, i64 -152, !revenant.va !1991
  %307 = ptrtoint ptr %fp998 to i64, !revenant.va !1991
  store i64 %307, ptr %rdx, align 8, !revenant.va !1991
  %fp999 = getelementptr inbounds i8, ptr %sp0, i64 -88, !revenant.va !1992
  %308 = ptrtoint ptr %fp999 to i64, !revenant.va !1992
  store i64 %308, ptr %rcx, align 8, !revenant.va !1992
  %r1000 = load i64, ptr %rcx, align 8, !revenant.va !1993
  %r1001 = load i64, ptr %rdx, align 8, !revenant.va !1993
  %c1002 = call i32 @sub_140001CB0(i64 %r1000, i64 %r1001), !revenant.va !1993
  store i64 undef, ptr %rax, align 8, !revenant.va !1993
  store i64 undef, ptr %rcx, align 8, !revenant.va !1993
  store i64 undef, ptr %rdx, align 8, !revenant.va !1993
  store i1 undef, ptr %cf, align 1, !revenant.va !1993
  store i1 undef, ptr %of, align 1, !revenant.va !1993
  store i1 undef, ptr %pf, align 1, !revenant.va !1993
  store i1 undef, ptr %sf, align 1, !revenant.va !1993
  store i1 undef, ptr %zf, align 1, !revenant.va !1993
  %e1003 = zext i32 %c1002 to i64, !revenant.va !1993
  store i64 %e1003, ptr %rax, align 8, !revenant.va !1993
  %fp1004 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1994
  %309 = ptrtoint ptr %fp1004 to i64, !revenant.va !1994
  %310 = load i32, ptr %fp1004, align 1, !revenant.va !1994
  %r1005 = load i64, ptr %rax, align 8, !revenant.va !1994
  %t1006 = trunc i64 %r1005 to i32, !revenant.va !1994
  %311 = add i32 %t1006, %310, !revenant.va !1994
  %zf1007 = icmp eq i32 %311, 0, !revenant.va !1994
  store i1 %zf1007, ptr %zf, align 1, !revenant.va !1994
  %sf1008 = and i32 %311, -2147483648, !revenant.va !1994
  %sf1009 = icmp ne i32 %sf1008, 0, !revenant.va !1994
  store i1 %sf1009, ptr %sf, align 1, !revenant.va !1994
  %l1010 = trunc i32 %311 to i8, !revenant.va !1994
  %312 = call i8 @llvm.ctpop.i8(i8 %l1010), !revenant.va !1994
  %pf1011 = trunc i8 %312 to i1, !revenant.va !1994
  %pf1012 = xor i1 %pf1011, true, !revenant.va !1994
  store i1 %pf1012, ptr %pf, align 1, !revenant.va !1994
  %cf1013 = icmp ult i32 %311, %t1006, !revenant.va !1994
  store i1 %cf1013, ptr %cf, align 1, !revenant.va !1994
  %o1014 = xor i32 %t1006, %310, !revenant.va !1994
  %n1015 = xor i32 %o1014, -1, !revenant.va !1994
  %o21016 = xor i32 %t1006, %311, !revenant.va !1994
  %o31017 = and i32 %n1015, %o21016, !revenant.va !1994
  %o41018 = and i32 %o31017, -2147483648, !revenant.va !1994
  %of1019 = icmp ne i32 %o41018, 0, !revenant.va !1994
  store i1 %of1019, ptr %of, align 1, !revenant.va !1994
  %z1020 = zext i32 %311 to i64, !revenant.va !1994
  store i64 %z1020, ptr %rax, align 8, !revenant.va !1994
  %r1021 = load i64, ptr %rax, align 8, !revenant.va !1995
  %t1022 = trunc i64 %r1021 to i32, !revenant.va !1995
  %fp1023 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1995
  %313 = ptrtoint ptr %fp1023 to i64, !revenant.va !1995
  store i32 %t1022, ptr %fp1023, align 1, !revenant.va !1995
  %fp1024 = getelementptr inbounds i8, ptr %sp0, i64 -88, !revenant.va !1996
  %314 = ptrtoint ptr %fp1024 to i64, !revenant.va !1996
  store i64 %314, ptr %rcx, align 8, !revenant.va !1996
  store i64 16, ptr %rdx, align 8, !revenant.va !1997
  %r1025 = load i64, ptr %rcx, align 8, !revenant.va !1998
  %r1026 = load i64, ptr %rdx, align 8, !revenant.va !1998
  %e1027 = trunc i64 %r1026 to i32, !revenant.va !1998
  %c1028 = call i32 @sub_140001D10(i64 %r1025, i32 %e1027), !revenant.va !1998
  store i64 undef, ptr %rax, align 8, !revenant.va !1998
  store i64 undef, ptr %rcx, align 8, !revenant.va !1998
  store i64 undef, ptr %rdx, align 8, !revenant.va !1998
  store i64 undef, ptr %r8, align 8, !revenant.va !1998
  store i64 undef, ptr %r9, align 8, !revenant.va !1998
  store i64 undef, ptr %r10, align 8, !revenant.va !1998
  store i64 undef, ptr %r11, align 8, !revenant.va !1998
  store i128 undef, ptr %xmm0, align 16, !revenant.va !1998
  store i128 undef, ptr %xmm1, align 16, !revenant.va !1998
  store i128 undef, ptr %xmm2, align 16, !revenant.va !1998
  store i128 undef, ptr %xmm3, align 16, !revenant.va !1998
  store i128 undef, ptr %xmm4, align 16, !revenant.va !1998
  store i128 undef, ptr %xmm5, align 16, !revenant.va !1998
  store i1 undef, ptr %cf, align 1, !revenant.va !1998
  store i1 undef, ptr %of, align 1, !revenant.va !1998
  store i1 undef, ptr %pf, align 1, !revenant.va !1998
  store i1 undef, ptr %sf, align 1, !revenant.va !1998
  store i1 undef, ptr %zf, align 1, !revenant.va !1998
  %e1029 = zext i32 %c1028 to i64, !revenant.va !1998
  store i64 %e1029, ptr %rax, align 8, !revenant.va !1998
  %fp1030 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !1999
  %315 = ptrtoint ptr %fp1030 to i64, !revenant.va !1999
  %316 = load i32, ptr %fp1030, align 1, !revenant.va !1999
  %r1031 = load i64, ptr %rax, align 8, !revenant.va !1999
  %t1032 = trunc i64 %r1031 to i32, !revenant.va !1999
  %317 = add i32 %t1032, %316, !revenant.va !1999
  %zf1033 = icmp eq i32 %317, 0, !revenant.va !1999
  store i1 %zf1033, ptr %zf, align 1, !revenant.va !1999
  %sf1034 = and i32 %317, -2147483648, !revenant.va !1999
  %sf1035 = icmp ne i32 %sf1034, 0, !revenant.va !1999
  store i1 %sf1035, ptr %sf, align 1, !revenant.va !1999
  %l1036 = trunc i32 %317 to i8, !revenant.va !1999
  %318 = call i8 @llvm.ctpop.i8(i8 %l1036), !revenant.va !1999
  %pf1037 = trunc i8 %318 to i1, !revenant.va !1999
  %pf1038 = xor i1 %pf1037, true, !revenant.va !1999
  store i1 %pf1038, ptr %pf, align 1, !revenant.va !1999
  %cf1039 = icmp ult i32 %317, %t1032, !revenant.va !1999
  store i1 %cf1039, ptr %cf, align 1, !revenant.va !1999
  %o1040 = xor i32 %t1032, %316, !revenant.va !1999
  %n1041 = xor i32 %o1040, -1, !revenant.va !1999
  %o21042 = xor i32 %t1032, %317, !revenant.va !1999
  %o31043 = and i32 %n1041, %o21042, !revenant.va !1999
  %o41044 = and i32 %o31043, -2147483648, !revenant.va !1999
  %of1045 = icmp ne i32 %o41044, 0, !revenant.va !1999
  store i1 %of1045, ptr %of, align 1, !revenant.va !1999
  %z1046 = zext i32 %317 to i64, !revenant.va !1999
  store i64 %z1046, ptr %rax, align 8, !revenant.va !1999
  %r1047 = load i64, ptr %rax, align 8, !revenant.va !2000
  %t1048 = trunc i64 %r1047 to i32, !revenant.va !2000
  %fp1049 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !2000
  %319 = ptrtoint ptr %fp1049 to i64, !revenant.va !2000
  store i32 %t1048, ptr %fp1049, align 1, !revenant.va !2000
  store i64 48, ptr %rcx, align 8, !revenant.va !2001
  store i64 36, ptr %rdx, align 8, !revenant.va !2002
  %r1050 = load i64, ptr %rcx, align 8, !revenant.va !2003
  %e1051 = trunc i64 %r1050 to i32, !revenant.va !2003
  %r1052 = load i64, ptr %rdx, align 8, !revenant.va !2003
  %e1053 = trunc i64 %r1052 to i32, !revenant.va !2003
  %c1054 = call i32 @sub_140001D70(i32 %e1051, i32 %e1053), !revenant.va !2003
  store i64 undef, ptr %rax, align 8, !revenant.va !2003
  store i64 undef, ptr %rcx, align 8, !revenant.va !2003
  store i64 undef, ptr %rdx, align 8, !revenant.va !2003
  store i64 undef, ptr %r8, align 8, !revenant.va !2003
  store i64 undef, ptr %r9, align 8, !revenant.va !2003
  store i64 undef, ptr %r10, align 8, !revenant.va !2003
  store i64 undef, ptr %r11, align 8, !revenant.va !2003
  store i128 undef, ptr %xmm0, align 16, !revenant.va !2003
  store i128 undef, ptr %xmm1, align 16, !revenant.va !2003
  store i128 undef, ptr %xmm2, align 16, !revenant.va !2003
  store i128 undef, ptr %xmm3, align 16, !revenant.va !2003
  store i128 undef, ptr %xmm4, align 16, !revenant.va !2003
  store i128 undef, ptr %xmm5, align 16, !revenant.va !2003
  store i1 undef, ptr %cf, align 1, !revenant.va !2003
  store i1 undef, ptr %of, align 1, !revenant.va !2003
  store i1 undef, ptr %pf, align 1, !revenant.va !2003
  store i1 undef, ptr %sf, align 1, !revenant.va !2003
  store i1 undef, ptr %zf, align 1, !revenant.va !2003
  %e1055 = zext i32 %c1054 to i64, !revenant.va !2003
  store i64 %e1055, ptr %rax, align 8, !revenant.va !2003
  %fp1056 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !2004
  %320 = ptrtoint ptr %fp1056 to i64, !revenant.va !2004
  %321 = load i32, ptr %fp1056, align 1, !revenant.va !2004
  %r1057 = load i64, ptr %rax, align 8, !revenant.va !2004
  %t1058 = trunc i64 %r1057 to i32, !revenant.va !2004
  %322 = add i32 %t1058, %321, !revenant.va !2004
  %zf1059 = icmp eq i32 %322, 0, !revenant.va !2004
  store i1 %zf1059, ptr %zf, align 1, !revenant.va !2004
  %sf1060 = and i32 %322, -2147483648, !revenant.va !2004
  %sf1061 = icmp ne i32 %sf1060, 0, !revenant.va !2004
  store i1 %sf1061, ptr %sf, align 1, !revenant.va !2004
  %l1062 = trunc i32 %322 to i8, !revenant.va !2004
  %323 = call i8 @llvm.ctpop.i8(i8 %l1062), !revenant.va !2004
  %pf1063 = trunc i8 %323 to i1, !revenant.va !2004
  %pf1064 = xor i1 %pf1063, true, !revenant.va !2004
  store i1 %pf1064, ptr %pf, align 1, !revenant.va !2004
  %cf1065 = icmp ult i32 %322, %t1058, !revenant.va !2004
  store i1 %cf1065, ptr %cf, align 1, !revenant.va !2004
  %o1066 = xor i32 %t1058, %321, !revenant.va !2004
  %n1067 = xor i32 %o1066, -1, !revenant.va !2004
  %o21068 = xor i32 %t1058, %322, !revenant.va !2004
  %o31069 = and i32 %n1067, %o21068, !revenant.va !2004
  %o41070 = and i32 %o31069, -2147483648, !revenant.va !2004
  %of1071 = icmp ne i32 %o41070, 0, !revenant.va !2004
  store i1 %of1071, ptr %of, align 1, !revenant.va !2004
  %z1072 = zext i32 %322 to i64, !revenant.va !2004
  store i64 %z1072, ptr %rax, align 8, !revenant.va !2004
  %r1073 = load i64, ptr %rax, align 8, !revenant.va !2005
  %t1074 = trunc i64 %r1073 to i32, !revenant.va !2005
  %fp1075 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !2005
  %324 = ptrtoint ptr %fp1075 to i64, !revenant.va !2005
  store i32 %t1074, ptr %fp1075, align 1, !revenant.va !2005
  store i64 10, ptr %rcx, align 8, !revenant.va !2006
  %r1076 = load i64, ptr %rdx, align 8, !revenant.va !2007
  %t1077 = trunc i64 %r1076 to i32, !revenant.va !2007
  %r1078 = load i64, ptr %rdx, align 8, !revenant.va !2007
  %t1079 = trunc i64 %r1078 to i32, !revenant.va !2007
  %325 = xor i32 %t1079, %t1077, !revenant.va !2007
  %zf1080 = icmp eq i32 %325, 0, !revenant.va !2007
  store i1 %zf1080, ptr %zf, align 1, !revenant.va !2007
  %sf1081 = and i32 %325, -2147483648, !revenant.va !2007
  %sf1082 = icmp ne i32 %sf1081, 0, !revenant.va !2007
  store i1 %sf1082, ptr %sf, align 1, !revenant.va !2007
  %l1083 = trunc i32 %325 to i8, !revenant.va !2007
  %326 = call i8 @llvm.ctpop.i8(i8 %l1083), !revenant.va !2007
  %pf1084 = trunc i8 %326 to i1, !revenant.va !2007
  %pf1085 = xor i1 %pf1084, true, !revenant.va !2007
  store i1 %pf1085, ptr %pf, align 1, !revenant.va !2007
  store i1 false, ptr %cf, align 1, !revenant.va !2007
  store i1 false, ptr %of, align 1, !revenant.va !2007
  %z1086 = zext i32 %325 to i64, !revenant.va !2007
  store i64 %z1086, ptr %rdx, align 8, !revenant.va !2007
  %r1087 = load i64, ptr %rcx, align 8, !revenant.va !2008
  %e1088 = trunc i64 %r1087 to i32, !revenant.va !2008
  %r1089 = load i64, ptr %rdx, align 8, !revenant.va !2008
  %e1090 = trunc i64 %r1089 to i32, !revenant.va !2008
  %c1091 = call i32 @sub_140001DB0(i32 %e1088, i32 %e1090), !revenant.va !2008
  store i64 undef, ptr %rax, align 8, !revenant.va !2008
  store i64 undef, ptr %rcx, align 8, !revenant.va !2008
  store i64 undef, ptr %rdx, align 8, !revenant.va !2008
  store i64 undef, ptr %r8, align 8, !revenant.va !2008
  store i64 undef, ptr %r9, align 8, !revenant.va !2008
  store i64 undef, ptr %r10, align 8, !revenant.va !2008
  store i64 undef, ptr %r11, align 8, !revenant.va !2008
  store i128 undef, ptr %xmm0, align 16, !revenant.va !2008
  store i128 undef, ptr %xmm1, align 16, !revenant.va !2008
  store i128 undef, ptr %xmm2, align 16, !revenant.va !2008
  store i128 undef, ptr %xmm3, align 16, !revenant.va !2008
  store i128 undef, ptr %xmm4, align 16, !revenant.va !2008
  store i128 undef, ptr %xmm5, align 16, !revenant.va !2008
  store i1 undef, ptr %cf, align 1, !revenant.va !2008
  store i1 undef, ptr %of, align 1, !revenant.va !2008
  store i1 undef, ptr %pf, align 1, !revenant.va !2008
  store i1 undef, ptr %sf, align 1, !revenant.va !2008
  store i1 undef, ptr %zf, align 1, !revenant.va !2008
  %e1092 = zext i32 %c1091 to i64, !revenant.va !2008
  store i64 %e1092, ptr %rax, align 8, !revenant.va !2008
  %fp1093 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !2009
  %327 = ptrtoint ptr %fp1093 to i64, !revenant.va !2009
  %328 = load i32, ptr %fp1093, align 1, !revenant.va !2009
  %r1094 = load i64, ptr %rax, align 8, !revenant.va !2009
  %t1095 = trunc i64 %r1094 to i32, !revenant.va !2009
  %329 = add i32 %t1095, %328, !revenant.va !2009
  %zf1096 = icmp eq i32 %329, 0, !revenant.va !2009
  store i1 %zf1096, ptr %zf, align 1, !revenant.va !2009
  %sf1097 = and i32 %329, -2147483648, !revenant.va !2009
  %sf1098 = icmp ne i32 %sf1097, 0, !revenant.va !2009
  store i1 %sf1098, ptr %sf, align 1, !revenant.va !2009
  %l1099 = trunc i32 %329 to i8, !revenant.va !2009
  %330 = call i8 @llvm.ctpop.i8(i8 %l1099), !revenant.va !2009
  %pf1100 = trunc i8 %330 to i1, !revenant.va !2009
  %pf1101 = xor i1 %pf1100, true, !revenant.va !2009
  store i1 %pf1101, ptr %pf, align 1, !revenant.va !2009
  %cf1102 = icmp ult i32 %329, %t1095, !revenant.va !2009
  store i1 %cf1102, ptr %cf, align 1, !revenant.va !2009
  %o1103 = xor i32 %t1095, %328, !revenant.va !2009
  %n1104 = xor i32 %o1103, -1, !revenant.va !2009
  %o21105 = xor i32 %t1095, %329, !revenant.va !2009
  %o31106 = and i32 %n1104, %o21105, !revenant.va !2009
  %o41107 = and i32 %o31106, -2147483648, !revenant.va !2009
  %of1108 = icmp ne i32 %o41107, 0, !revenant.va !2009
  store i1 %of1108, ptr %of, align 1, !revenant.va !2009
  %z1109 = zext i32 %329 to i64, !revenant.va !2009
  store i64 %z1109, ptr %rax, align 8, !revenant.va !2009
  %r1110 = load i64, ptr %rax, align 8, !revenant.va !2010
  %t1111 = trunc i64 %r1110 to i32, !revenant.va !2010
  %fp1112 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !2010
  %331 = ptrtoint ptr %fp1112 to i64, !revenant.va !2010
  store i32 %t1111, ptr %fp1112, align 1, !revenant.va !2010
  %fp1113 = getelementptr inbounds i8, ptr %sp0, i64 -204, !revenant.va !2011
  %332 = ptrtoint ptr %fp1113 to i64, !revenant.va !2011
  %333 = load i32, ptr %fp1113, align 1, !revenant.va !2011
  %z1114 = zext i32 %333 to i64, !revenant.va !2011
  store i64 %z1114, ptr %rax, align 8, !revenant.va !2011
  %fp1115 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !2012
  %334 = ptrtoint ptr %fp1115 to i64, !revenant.va !2012
  %335 = load i32, ptr %fp1115, align 1, !revenant.va !2012
  %r1116 = load i64, ptr %rax, align 8, !revenant.va !2012
  %t1117 = trunc i64 %r1116 to i32, !revenant.va !2012
  %336 = add i32 %t1117, %335, !revenant.va !2012
  %zf1118 = icmp eq i32 %336, 0, !revenant.va !2012
  store i1 %zf1118, ptr %zf, align 1, !revenant.va !2012
  %sf1119 = and i32 %336, -2147483648, !revenant.va !2012
  %sf1120 = icmp ne i32 %sf1119, 0, !revenant.va !2012
  store i1 %sf1120, ptr %sf, align 1, !revenant.va !2012
  %l1121 = trunc i32 %336 to i8, !revenant.va !2012
  %337 = call i8 @llvm.ctpop.i8(i8 %l1121), !revenant.va !2012
  %pf1122 = trunc i8 %337 to i1, !revenant.va !2012
  %pf1123 = xor i1 %pf1122, true, !revenant.va !2012
  store i1 %pf1123, ptr %pf, align 1, !revenant.va !2012
  %cf1124 = icmp ult i32 %336, %t1117, !revenant.va !2012
  store i1 %cf1124, ptr %cf, align 1, !revenant.va !2012
  %o1125 = xor i32 %t1117, %335, !revenant.va !2012
  %n1126 = xor i32 %o1125, -1, !revenant.va !2012
  %o21127 = xor i32 %t1117, %336, !revenant.va !2012
  %o31128 = and i32 %n1126, %o21127, !revenant.va !2012
  %o41129 = and i32 %o31128, -2147483648, !revenant.va !2012
  %of1130 = icmp ne i32 %o41129, 0, !revenant.va !2012
  store i1 %of1130, ptr %of, align 1, !revenant.va !2012
  %z1131 = zext i32 %336 to i64, !revenant.va !2012
  store i64 %z1131, ptr %rax, align 8, !revenant.va !2012
  %r1132 = load i64, ptr %rax, align 8, !revenant.va !2013
  %t1133 = trunc i64 %r1132 to i32, !revenant.va !2013
  %fp1134 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !2013
  %338 = ptrtoint ptr %fp1134 to i64, !revenant.va !2013
  store i32 %t1133, ptr %fp1134, align 1, !revenant.va !2013
  %fp1135 = getelementptr inbounds i8, ptr %sp0, i64 -320, !revenant.va !2014
  %339 = ptrtoint ptr %fp1135 to i64, !revenant.va !2014
  %340 = load i32, ptr %fp1135, align 1, !revenant.va !2014
  %z1136 = zext i32 %340 to i64, !revenant.va !2014
  store i64 %z1136, ptr %rax, align 8, !revenant.va !2014
  %341 = load i32, ptr @g_140085000, align 1, !revenant.va !2015
  %r1137 = load i64, ptr %rax, align 8, !revenant.va !2015
  %t1138 = trunc i64 %r1137 to i32, !revenant.va !2015
  %342 = add i32 %t1138, %341, !revenant.va !2015
  %zf1139 = icmp eq i32 %342, 0, !revenant.va !2015
  store i1 %zf1139, ptr %zf, align 1, !revenant.va !2015
  %sf1140 = and i32 %342, -2147483648, !revenant.va !2015
  %sf1141 = icmp ne i32 %sf1140, 0, !revenant.va !2015
  store i1 %sf1141, ptr %sf, align 1, !revenant.va !2015
  %l1142 = trunc i32 %342 to i8, !revenant.va !2015
  %343 = call i8 @llvm.ctpop.i8(i8 %l1142), !revenant.va !2015
  %pf1143 = trunc i8 %343 to i1, !revenant.va !2015
  %pf1144 = xor i1 %pf1143, true, !revenant.va !2015
  store i1 %pf1144, ptr %pf, align 1, !revenant.va !2015
  %cf1145 = icmp ult i32 %342, %t1138, !revenant.va !2015
  store i1 %cf1145, ptr %cf, align 1, !revenant.va !2015
  %o1146 = xor i32 %t1138, %341, !revenant.va !2015
  %n1147 = xor i32 %o1146, -1, !revenant.va !2015
  %o21148 = xor i32 %t1138, %342, !revenant.va !2015
  %o31149 = and i32 %n1147, %o21148, !revenant.va !2015
  %o41150 = and i32 %o31149, -2147483648, !revenant.va !2015
  %of1151 = icmp ne i32 %o41150, 0, !revenant.va !2015
  store i1 %of1151, ptr %of, align 1, !revenant.va !2015
  %z1152 = zext i32 %342 to i64, !revenant.va !2015
  store i64 %z1152, ptr %rax, align 8, !revenant.va !2015
  %r1153 = load i64, ptr %rax, align 8, !revenant.va !2016
  %t1154 = trunc i64 %r1153 to i32, !revenant.va !2016
  %fp1155 = getelementptr inbounds i8, ptr %sp0, i64 -336, !revenant.va !2016
  %344 = ptrtoint ptr %fp1155 to i64, !revenant.va !2016
  store i32 %t1154, ptr %fp1155, align 1, !revenant.va !2016
  store i64 1, ptr %rcx, align 8, !revenant.va !2017
  %r1156 = load i64, ptr %rcx, align 8, !revenant.va !2018
  %e1157 = trunc i64 %r1156 to i32, !revenant.va !2018
  %c1158 = call i64 @sub_1400019D0(i32 %e1157), !revenant.va !2018
  store i64 undef, ptr %rax, align 8, !revenant.va !2018
  store i64 undef, ptr %rcx, align 8, !revenant.va !2018
  store i64 undef, ptr %rdx, align 8, !revenant.va !2018
  store i1 undef, ptr %cf, align 1, !revenant.va !2018
  store i1 undef, ptr %of, align 1, !revenant.va !2018
  store i1 undef, ptr %pf, align 1, !revenant.va !2018
  store i1 undef, ptr %sf, align 1, !revenant.va !2018
  store i1 undef, ptr %zf, align 1, !revenant.va !2018
  store i64 %c1158, ptr %rax, align 8, !revenant.va !2018
  %fp1159 = getelementptr inbounds i8, ptr %sp0, i64 -336, !revenant.va !2019
  %345 = ptrtoint ptr %fp1159 to i64, !revenant.va !2019
  %346 = load i32, ptr %fp1159, align 1, !revenant.va !2019
  %z1160 = zext i32 %346 to i64, !revenant.va !2019
  store i64 %z1160, ptr %r8, align 8, !revenant.va !2019
  %r1161 = load i64, ptr %rax, align 8, !revenant.va !2020
  store i64 %r1161, ptr %rdx, align 8, !revenant.va !2020
  store i64 ptrtoint (ptr @g_1400710f7 to i64), ptr %rcx, align 8, !revenant.va !2021
  %r1162 = load i64, ptr %rcx, align 8, !revenant.va !2022
  %r1163 = load i64, ptr %rdx, align 8, !revenant.va !2022
  %r1164 = load i64, ptr %r8, align 8, !revenant.va !2022
  %r1165 = load i64, ptr %r9, align 8, !revenant.va !2022
  %c1166 = call i32 @sub_140002400(i64 %r1162, i64 %r1163, i64 %r1164, i64 %r1165), !revenant.va !2022
  store i64 undef, ptr %rax, align 8, !revenant.va !2022
  store i64 undef, ptr %rcx, align 8, !revenant.va !2022
  store i64 undef, ptr %rdx, align 8, !revenant.va !2022
  store i64 undef, ptr %r8, align 8, !revenant.va !2022
  store i64 undef, ptr %r9, align 8, !revenant.va !2022
  store i64 undef, ptr %r10, align 8, !revenant.va !2022
  store i64 undef, ptr %r11, align 8, !revenant.va !2022
  store i128 undef, ptr %xmm0, align 16, !revenant.va !2022
  store i128 undef, ptr %xmm1, align 16, !revenant.va !2022
  store i128 undef, ptr %xmm2, align 16, !revenant.va !2022
  store i128 undef, ptr %xmm3, align 16, !revenant.va !2022
  store i128 undef, ptr %xmm4, align 16, !revenant.va !2022
  store i128 undef, ptr %xmm5, align 16, !revenant.va !2022
  store i1 undef, ptr %cf, align 1, !revenant.va !2022
  store i1 undef, ptr %of, align 1, !revenant.va !2022
  store i1 undef, ptr %pf, align 1, !revenant.va !2022
  store i1 undef, ptr %sf, align 1, !revenant.va !2022
  store i1 undef, ptr %zf, align 1, !revenant.va !2022
  %e1167 = zext i32 %c1166 to i64, !revenant.va !2022
  store i64 %e1167, ptr %rax, align 8, !revenant.va !2022
  %r1168 = load i64, ptr %rax, align 8, !revenant.va !2023
  %t1169 = trunc i64 %r1168 to i32, !revenant.va !2023
  %r1170 = load i64, ptr %rax, align 8, !revenant.va !2023
  %t1171 = trunc i64 %r1170 to i32, !revenant.va !2023
  %347 = xor i32 %t1171, %t1169, !revenant.va !2023
  %zf1172 = icmp eq i32 %347, 0, !revenant.va !2023
  store i1 %zf1172, ptr %zf, align 1, !revenant.va !2023
  %sf1173 = and i32 %347, -2147483648, !revenant.va !2023
  %sf1174 = icmp ne i32 %sf1173, 0, !revenant.va !2023
  store i1 %sf1174, ptr %sf, align 1, !revenant.va !2023
  %l1175 = trunc i32 %347 to i8, !revenant.va !2023
  %348 = call i8 @llvm.ctpop.i8(i8 %l1175), !revenant.va !2023
  %pf1176 = trunc i8 %348 to i1, !revenant.va !2023
  %pf1177 = xor i1 %pf1176, true, !revenant.va !2023
  store i1 %pf1177, ptr %pf, align 1, !revenant.va !2023
  store i1 false, ptr %cf, align 1, !revenant.va !2023
  store i1 false, ptr %of, align 1, !revenant.va !2023
  %z1178 = zext i32 %347 to i64, !revenant.va !2023
  store i64 %z1178, ptr %rax, align 8, !revenant.va !2023
  %r1179 = load i64, ptr %rax, align 8, !revenant.va !2024
  %t1180 = trunc i64 %r1179 to i32, !revenant.va !2024
  %fp1181 = getelementptr inbounds i8, ptr %sp0, i64 -332, !revenant.va !2024
  %349 = ptrtoint ptr %fp1181 to i64, !revenant.va !2024
  store i32 %t1180, ptr %fp1181, align 1, !revenant.va !2024
  %fp1182 = getelementptr inbounds i8, ptr %sp0, i64 -8, !revenant.va !2025
  %350 = ptrtoint ptr %fp1182 to i64, !revenant.va !2025
  %351 = load i64, ptr %fp1182, align 1, !revenant.va !2025
  store i64 %351, ptr %rcx, align 8, !revenant.va !2025
  %fp1183 = getelementptr inbounds i8, ptr %sp0, i64 -376, !revenant.va !2026
  %rsp1184 = ptrtoint ptr %fp1183 to i64, !revenant.va !2026
  %r1185 = load i64, ptr %rcx, align 8, !revenant.va !2026
  %352 = xor i64 %r1185, %rsp1184, !revenant.va !2026
  %zf1186 = icmp eq i64 %352, 0, !revenant.va !2026
  store i1 %zf1186, ptr %zf, align 1, !revenant.va !2026
  %sf1187 = and i64 %352, -9223372036854775808, !revenant.va !2026
  %sf1188 = icmp ne i64 %sf1187, 0, !revenant.va !2026
  store i1 %sf1188, ptr %sf, align 1, !revenant.va !2026
  %l1189 = trunc i64 %352 to i8, !revenant.va !2026
  %353 = call i8 @llvm.ctpop.i8(i8 %l1189), !revenant.va !2026
  %pf1190 = trunc i8 %353 to i1, !revenant.va !2026
  %pf1191 = xor i1 %pf1190, true, !revenant.va !2026
  store i1 %pf1191, ptr %pf, align 1, !revenant.va !2026
  store i1 false, ptr %cf, align 1, !revenant.va !2026
  store i1 false, ptr %of, align 1, !revenant.va !2026
  store i64 %352, ptr %rcx, align 8, !revenant.va !2026
  %354 = load i64, ptr @g_140085040.2, align 1, !revenant.va !2027
  store i64 %354, ptr %rax, align 8, !revenant.va !2027
  %r1192 = load i64, ptr %rcx, align 8, !revenant.va !2028
  %r1193 = load i64, ptr %rax, align 8, !revenant.va !2028
  %355 = sub i64 %r1193, %r1192, !revenant.va !2028
  %zf1194 = icmp eq i64 %355, 0, !revenant.va !2028
  store i1 %zf1194, ptr %zf, align 1, !revenant.va !2028
  %sf1195 = and i64 %355, -9223372036854775808, !revenant.va !2028
  %sf1196 = icmp ne i64 %sf1195, 0, !revenant.va !2028
  store i1 %sf1196, ptr %sf, align 1, !revenant.va !2028
  %l1197 = trunc i64 %355 to i8, !revenant.va !2028
  %356 = call i8 @llvm.ctpop.i8(i8 %l1197), !revenant.va !2028
  %pf1198 = trunc i8 %356 to i1, !revenant.va !2028
  %pf1199 = xor i1 %pf1198, true, !revenant.va !2028
  store i1 %pf1199, ptr %pf, align 1, !revenant.va !2028
  %cf1200 = icmp ult i64 %r1193, %r1192, !revenant.va !2028
  store i1 %cf1200, ptr %cf, align 1, !revenant.va !2028
  %o1201 = xor i64 %r1193, %r1192, !revenant.va !2028
  %o21202 = xor i64 %r1193, %355, !revenant.va !2028
  %o31203 = and i64 %o1201, %o21202, !revenant.va !2028
  %o41204 = and i64 %o31203, -9223372036854775808, !revenant.va !2028
  %of1205 = icmp ne i64 %o41204, 0, !revenant.va !2028
  store i1 %of1205, ptr %of, align 1, !revenant.va !2028
  store i64 %355, ptr %rax, align 8, !revenant.va !2028
  %c1206 = icmp ne i64 %355, 0, !revenant.va !1801
  br i1 %c1206, label %bb_14000223d, label %bb_14000223b, !revenant.va !2029

bb_14000223b:                                     ; preds = %bb_1400020bb
  br label %bb_14000224d, !revenant.va !2030

bb_14000223d:                                     ; preds = %bb_1400020bb
  %fp1207 = getelementptr inbounds i8, ptr %sp0, i64 -8, !revenant.va !2031
  %357 = ptrtoint ptr %fp1207 to i64, !revenant.va !2031
  %358 = load i64, ptr %fp1207, align 1, !revenant.va !2031
  store i64 %358, ptr %rcx, align 8, !revenant.va !2031
  %fp1208 = getelementptr inbounds i8, ptr %sp0, i64 -376, !revenant.va !2032
  %rsp1209 = ptrtoint ptr %fp1208 to i64, !revenant.va !2032
  %r1210 = load i64, ptr %rcx, align 8, !revenant.va !2032
  %359 = xor i64 %r1210, %rsp1209, !revenant.va !2032
  %zf1211 = icmp eq i64 %359, 0, !revenant.va !2032
  store i1 %zf1211, ptr %zf, align 1, !revenant.va !2032
  %sf1212 = and i64 %359, -9223372036854775808, !revenant.va !2032
  %sf1213 = icmp ne i64 %sf1212, 0, !revenant.va !2032
  store i1 %sf1213, ptr %sf, align 1, !revenant.va !2032
  %l1214 = trunc i64 %359 to i8, !revenant.va !2032
  %360 = call i8 @llvm.ctpop.i8(i8 %l1214), !revenant.va !2032
  %pf1215 = trunc i8 %360 to i1, !revenant.va !2032
  %pf1216 = xor i1 %pf1215, true, !revenant.va !2032
  store i1 %pf1216, ptr %pf, align 1, !revenant.va !2032
  store i1 false, ptr %cf, align 1, !revenant.va !2032
  store i1 false, ptr %of, align 1, !revenant.va !2032
  store i64 %359, ptr %rcx, align 8, !revenant.va !2032
  %r1217 = load i64, ptr %rcx, align 8, !revenant.va !2033
  call void @sub_140002640(i64 %r1217), !revenant.va !2033
  store i64 undef, ptr %rax, align 8, !revenant.va !2033
  store i64 undef, ptr %rcx, align 8, !revenant.va !2033
  store i64 undef, ptr %rdx, align 8, !revenant.va !2033
  store i64 undef, ptr %r8, align 8, !revenant.va !2033
  store i64 undef, ptr %r9, align 8, !revenant.va !2033
  store i64 undef, ptr %r10, align 8, !revenant.va !2033
  store i64 undef, ptr %r11, align 8, !revenant.va !2033
  store i128 undef, ptr %xmm0, align 16, !revenant.va !2033
  store i128 undef, ptr %xmm1, align 16, !revenant.va !2033
  store i128 undef, ptr %xmm2, align 16, !revenant.va !2033
  store i128 undef, ptr %xmm3, align 16, !revenant.va !2033
  store i128 undef, ptr %xmm4, align 16, !revenant.va !2033
  store i128 undef, ptr %xmm5, align 16, !revenant.va !2033
  store i1 undef, ptr %cf, align 1, !revenant.va !2033
  store i1 undef, ptr %of, align 1, !revenant.va !2033
  store i1 undef, ptr %pf, align 1, !revenant.va !2033
  store i1 undef, ptr %sf, align 1, !revenant.va !2033
  store i1 undef, ptr %zf, align 1, !revenant.va !2033
  br label %bb_14000224d, !revenant.va !2033

bb_14000224d:                                     ; preds = %bb_14000223d, %bb_14000223b
  %fp1218 = getelementptr inbounds i8, ptr %sp0, i64 -332, !revenant.va !2034
  %361 = ptrtoint ptr %fp1218 to i64, !revenant.va !2034
  %362 = load i32, ptr %fp1218, align 1, !revenant.va !2034
  %z1219 = zext i32 %362 to i64, !revenant.va !2034
  store i64 %z1219, ptr %rax, align 8, !revenant.va !2034
  %fp1220 = getelementptr inbounds i8, ptr %sp0, i64 -376, !revenant.va !2035
  %rsp1221 = ptrtoint ptr %fp1220 to i64, !revenant.va !2035
  %363 = add i64 %rsp1221, 376, !revenant.va !2035
  %zf1222 = icmp eq i64 %363, 0, !revenant.va !2035
  store i1 %zf1222, ptr %zf, align 1, !revenant.va !2035
  %sf1223 = and i64 %363, -9223372036854775808, !revenant.va !2035
  %sf1224 = icmp ne i64 %sf1223, 0, !revenant.va !2035
  store i1 %sf1224, ptr %sf, align 1, !revenant.va !2035
  %l1225 = trunc i64 %363 to i8, !revenant.va !2035
  %364 = call i8 @llvm.ctpop.i8(i8 %l1225), !revenant.va !2035
  %pf1226 = trunc i8 %364 to i1, !revenant.va !2035
  %pf1227 = xor i1 %pf1226, true, !revenant.va !2035
  store i1 %pf1227, ptr %pf, align 1, !revenant.va !2035
  %cf1228 = icmp ult i64 %363, %rsp1221, !revenant.va !2035
  store i1 %cf1228, ptr %cf, align 1, !revenant.va !2035
  %o1229 = xor i64 %rsp1221, 376, !revenant.va !2035
  %n1230 = xor i64 %o1229, -1, !revenant.va !2035
  %o21231 = xor i64 %rsp1221, %363, !revenant.va !2035
  %o31232 = and i64 %n1230, %o21231, !revenant.va !2035
  %o41233 = and i64 %o31232, -9223372036854775808, !revenant.va !2035
  %of1234 = icmp ne i64 %o41233, 0, !revenant.va !2035
  store i1 %of1234, ptr %of, align 1, !revenant.va !2035
  %r1235 = load i64, ptr %rax, align 8, !revenant.va !1801
  %e1236 = trunc i64 %r1235 to i32, !revenant.va !1801
  ret i32 %e1236, !revenant.va !1801
}
