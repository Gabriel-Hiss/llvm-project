//===- UnsroaPlugin.cpp - plugin-only entry point ----------------------===//
//
// Part of the Revenant project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Empty TU: the llvmGetPassPluginInfo entry lives in Unsroa.cpp behind
// REVENANT_BUILD_PLUGIN so the static revenant-opt link never sees it.
// This file only gives the MODULE target a second TU like the other
// transform slices carry (UnrotatePlugin.cpp); it contributes no code.
//
//===----------------------------------------------------------------------===//
