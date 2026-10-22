//===- Evidence.h - embedded-metadata evidence for deopt passes -*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// One evidence API that deopt passes query, built from embedded metadata
// and libdb. The lifter attaches per-function `rev.*` metadata from the
// analyzed Program (source Locations, panic/assert strings, EH types, RTTI
// classes), `revenant.lib` function attributes from libdb matches, and
// `!revenant.va` provenance on every instruction. This header reads those
// attachments back as evidence for Reroll/UnrollFull (unroll factors) and
// Uninline (standalone-body matches).
//
// Rules, agreed with the pass owners:
// - Evidence only confirms a rewrite the shape matcher already allows; it
//   never enables one on its own. Every query defaults to absent (nullopt,
//   false, empty) when the module carries no metadata, so `--meta=none`
//   output stays valid and correct.
// - Pure Function/Instruction/MDNode reads only: no analysis manager, no
//   module scans, cheap enough to call per fold candidate. Header-only so
//   statically linked plugin DLLs can include it with no link dependency.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_EVIDENCE_EVIDENCE_H
#define REVENANT_EVIDENCE_EVIDENCE_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"

namespace revenant::evidence {

// Function metadata slot carrying the lift-recorded unroll factor:
// `!rev.unroll !{i32 D}`, where D is the count of repeated asm windows
// sharing one source Location. See LiftFunction.cpp.
inline constexpr llvm::StringLiteral MDUnroll = "rev.unroll";
// Function metadata slot carrying cold-part chunk ranges:
// `!rev.chunks !{!{i64 start, i64 end}, ...}` for functions whose code is
// split across several address ranges.
inline constexpr llvm::StringLiteral MDChunks = "rev.chunks";
// Function metadata slots the lifter already attaches from Embedded.
inline constexpr llvm::StringLiteral MDSource = "rev.source";
inline constexpr llvm::StringLiteral MDPanic = "rev.panic";
inline constexpr llvm::StringLiteral MDEHTypes = "rev.eh.types";
inline constexpr llvm::StringLiteral MDClass = "rev.class";
inline constexpr llvm::StringLiteral MDStrings = "rev.strings";
// Function attribute carrying the libdb match name (Stages.cpp).
inline constexpr llvm::StringLiteral FnAttrLib = "revenant.lib";

namespace detail {

// i32 operand of a single-element metadata node, if well formed.
inline std::optional<int32_t> i32Of(const llvm::MDNode *N) {
  if (!N || N->getNumOperands() != 1)
    return std::nullopt;
  auto *CI = llvm::mdconst::dyn_extract<llvm::ConstantInt>(N->getOperand(0));
  if (!CI || !CI->getType()->isIntegerTy(32))
    return std::nullopt;
  return static_cast<int32_t>(CI->getSExtValue());
}

// All MDString operands of a function metadata node.
inline std::vector<std::string> stringsOf(const llvm::Function &F,
                                          llvm::StringRef Kind) {
  std::vector<std::string> Out;
  llvm::MDNode *N = F.getMetadata(Kind);
  if (!N)
    return Out;
  for (const llvm::MDOperand &Op : N->operands())
    if (auto *S = llvm::dyn_cast_or_null<llvm::MDString>(Op.get()))
      Out.push_back(S->getString().str());
  return Out;
}

} // namespace detail

// Returns the unroll factor recorded at lift, or nullopt when the module
// carries no evidence for this function. Never guesses: absent or
// malformed metadata means absent evidence.
inline std::optional<unsigned>
unrollFactor(const llvm::Function &F) {
  std::optional<int32_t> D = detail::i32Of(F.getMetadata(MDUnroll));
  if (!D || *D < 2)
    return std::nullopt;
  return static_cast<unsigned>(*D);
}

// Distinctive references of a callee for inline-remnant disambiguation:
// the source Location the lifter recorded, the crash-metadata strings it
// references, and the libdb match name. Empty when the module carries no
// evidence. Uninline compares these against the candidate region's own
// references; evidence only lifts the ambiguity veto on matches the
// structural matcher already found.
struct InlineEvidence {
  // "path:line:col" when the lifter recorded a source Location.
  std::optional<std::string> Location;
  // Panic/assert/file strings referenced by the callee.
  std::vector<std::string> Strings;
  // libdb match name (`revenant.lib` attribute), if any.
  std::optional<std::string> Library;
  bool empty() const {
    return !Location && Strings.empty() && !Library;
  }
};

inline InlineEvidence uninlineEvidence(const llvm::Function &Callee) {
  InlineEvidence Out;
  if (llvm::MDNode *N = Callee.getMetadata(MDSource)) {
    if (N->getNumOperands() >= 3) {
      if (auto *S = llvm::dyn_cast_or_null<llvm::MDString>(N->getOperand(0))) {
        auto *Line =
            llvm::mdconst::dyn_extract<llvm::ConstantInt>(N->getOperand(1));
        auto *Col =
            llvm::mdconst::dyn_extract<llvm::ConstantInt>(N->getOperand(2));
        if (Line && Col && Line->getType()->isIntegerTy(32) &&
            Col->getType()->isIntegerTy(32))
          Out.Location = (S->getString() + ":" + llvm::Twine(Line->getZExtValue()) +
                          ":" + llvm::Twine(Col->getZExtValue()))
                             .str();
      }
    }
  }
  Out.Strings = detail::stringsOf(Callee, MDStrings);
  for (const std::string &Panic : detail::stringsOf(Callee, MDPanic))
    Out.Strings.push_back(Panic);
  if (Callee.hasFnAttribute(FnAttrLib))
    Out.Library = Callee.getFnAttribute(FnAttrLib).getValueAsString().str();
  return Out;
}

// Does a candidate inline region reference the same distinctive strings
// or library body as the callee's evidence? Checks string-global
// initializers in the region against the callee's rev.strings/rev.panic
// entries, and direct-call targets against its revenant.lib name. Pure
// reads over the given instructions only, false when either side carries
// no evidence. Location evidence has no region-side counterpart at IR
// level, so it never confirms on its own.
inline bool regionMatchesEvidence(const InlineEvidence &Ev,
                                  llvm::ArrayRef<llvm::Instruction *> Region) {
  if (Ev.Strings.empty() && !Ev.Library)
    return false;
  for (const llvm::Instruction *I : Region) {
    if (!I)
      continue;
    for (const llvm::Value *V : I->operands()) {
      if (const auto *G = llvm::dyn_cast<llvm::GlobalVariable>(
              V->stripPointerCasts())) {
        if (!G->hasInitializer())
          continue;
        if (const auto *Init =
                llvm::dyn_cast<llvm::ConstantDataArray>(G->getInitializer())) {
          if (!Init->isString())
            continue;
          for (const std::string &S : Ev.Strings)
            if (!S.empty() && Init->getAsString().contains(S))
              return true;
        }
      }
      if (const auto *CB = llvm::dyn_cast<llvm::CallBase>(I)) {
        if (const llvm::Function *T = CB->getCalledFunction()) {
          if (Ev.Library && T->hasFnAttribute(FnAttrLib) &&
              T->getFnAttribute(FnAttrLib).getValueAsString() == *Ev.Library)
            return true;
          if (Ev.Library && T->getName() == *Ev.Library)
            return true;
        }
      }
    }
  }
  return false;
}

// Chunk ranges of a function whose code is split across several address
// ranges (hot/cold partitions), as (start, end) VA pairs. Empty when the
// function is contiguous or the module carries no chunk metadata.
inline std::vector<std::pair<uint64_t, uint64_t>>
chunkRanges(const llvm::Function &F) {
  std::vector<std::pair<uint64_t, uint64_t>> Out;
  llvm::MDNode *N = F.getMetadata(MDChunks);
  if (!N)
    return Out;
  for (const llvm::MDOperand &Op : N->operands()) {
    auto *Range = llvm::dyn_cast_or_null<llvm::MDNode>(Op.get());
    if (!Range || Range->getNumOperands() != 2)
      continue;
    auto *Lo =
        llvm::mdconst::dyn_extract<llvm::ConstantInt>(Range->getOperand(0));
    auto *Hi =
        llvm::mdconst::dyn_extract<llvm::ConstantInt>(Range->getOperand(1));
    if (!Lo || !Hi || !Lo->getType()->isIntegerTy(64) ||
        !Hi->getType()->isIntegerTy(64))
      continue;
    if (Lo->getZExtValue() < Hi->getZExtValue())
      Out.emplace_back(Lo->getZExtValue(), Hi->getZExtValue());
  }
  return Out;
}

// EH region id of an instruction lifted inside a decoded try range
// (`!revenant.eh.region !{i32 id}`), if present.
inline std::optional<int32_t>
ehRegion(const llvm::Instruction &I) {
  return detail::i32Of(I.getMetadata("revenant.eh.region"));
}

} // namespace revenant::evidence

#endif // REVENANT_EVIDENCE_EVIDENCE_H
