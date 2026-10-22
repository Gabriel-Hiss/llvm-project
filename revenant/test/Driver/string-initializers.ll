; Global byte arrays print as string literals only when the implicit NUL fits
; the array; a redundant trailing NUL in the data is dropped and anything that
; still does not fit falls back to a braced initializer.
; RUN: %revenant-clang %s --lang c -o %t.c && %FileCheck %s --check-prefix=C < %t.c
; RUN: %revenant-clang %s --lang cpp -o %t.cpp && %FileCheck %s --check-prefix=CPP < %t.cpp

@aorigin = internal constant [6 x i8] c"origin"
@g_140004108 = internal constant [56 x i8] c"\F4@\00@\01\00\00\00\F06\00@\01\00\00\00p7\00@\01\00\00\00P8\00@\01\00\00\00\888\00@\01\00\00\00\D28\00@\01\00\00\00\E68\00@\01\00\00\00"
@ahello_20world = internal constant [11 x i8] c"hello world"

; C: char aorigin[6] = "origin";
; C: char g_140004108[56] = "\364@\000@\001\000\000\000
; C: char ahello_20world[11] = "hello world";

; CPP: unsigned char aorigin[6] = {111, 114, 105, 103, 105, 110};
; CPP: char g_140004108[56] = "\364@\000@\001\000\000\000
; CPP: unsigned char ahello_20world[11] = {104, 101, 108, 108, 111,
