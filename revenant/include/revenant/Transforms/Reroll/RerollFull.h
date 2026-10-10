//===- RerollFull.h - Full/partial unroll re-rolling ----------*- C++ -*-===//
//
// Part of the Revenant project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `revenant::tryRerollFull`: re-rolls fully-unrolled (straight-line) and
// partially-unrolled loops the in-loop folder (`tryFoldLoop`) and the
// pointer folder (`tryFoldPtrLoop`) in Reroll.cpp leave behind.
//
// The gap it fills: strength-reduced unrolls whose per-window values are
// *not* constant-shifted copies of one address. The corpus `arrays` case
// (c-clang-o2 `sub_140001330`, MSVC `arrays`, GCC `arrays`) initializes
// `local[i] = i * idx` for 8 iterations; the optimizer strength-reduces
// each lane to its own multiply/shift/lea (`mul idx,3`, `shl idx,2`, ...)
// writing byte-consecutive stack slots. No two windows share one constant
// delta, so the constant-shift synth (`trySynthesize`) cannot fire, and
// there is no loop at all, so the folders have nothing to fold.
//
// Matching is affine-value based: each window's stored value must be an
// affine function `c*k + d` of the window index `k` over the *same* leaf
// value (the loop-invariant factor), with a uniform step `c` across all
// windows. Addresses must be a uniform byte stride from one base. Every
// window's instructions must carry `!revenant.va` provenance in one shared
// VA range with a uniform per-window VA stride (the unroller emits one
// source statement's copies at consecutive addresses); this is evidence
// only, never semantics: `--meta=none` output is identical.
//
// `revenant::tryRerollRegions`: re-rolls N chained isomorphic
// single-entry/single-exit diamond regions (guard + optional grow + join
// with PHIs, e.g. Rust vec_ops' 8 push sequences) into one counted loop
// reusing region 0's blocks. Guard consts must be affine in the copy
// index, stored consts must fit a quadratic (solved from the first three
// copies, verified on all), grow blocks must call one identical callee
// with identical args.
//
// On success the run rewrites to a counted loop around window 0's body
// with the value operand rewired to `c*i + d` and returns the trip count.
// Anything ambiguous stays put.
//
// Owned by MuseUnrollFullR; StepRerollR owns Reroll.cpp and calls this
// from `RerollPass::run`.
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_REROLL_REROLLFULL_H
#define REVENANT_TRANSFORMS_REROLL_REROLLFULL_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"

namespace revenant {

// Re-roll a straight-line run of isomorphic statement groups in B into one
// counted loop. Returns the trip count K (>= 3) on success, 0 otherwise.
// Appends the preheader block name to Notes on success.
unsigned tryRerollFull(llvm::Function &F, llvm::BasicBlock &B,
                       llvm::SmallVectorImpl<llvm::StringRef> &Notes);

// Re-roll N chained isomorphic single-entry/single-exit diamond regions
// (guard + optional grow + join with PHIs) into one counted loop reusing
// region 0's blocks. Returns N on success, 0 otherwise.
unsigned tryRerollRegions(llvm::Function &F,
                          llvm::SmallVectorImpl<llvm::StringRef> &Notes);

} // namespace revenant

#endif
