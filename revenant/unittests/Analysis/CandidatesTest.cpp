//===- unittests/Analysis/CandidatesTest.cpp - candidate checks ---------===//
//
// Port intent of the candidate rules in crates/analysis/src/lib.rs
// `Program::analyze`: strong seeds (entry, executable non-forwarder
// exports, TLS callbacks, primary pdata begins), the initial weak set
// (reloc targets), and the weak-function rejection predicate (all-zero and
// filler-only functions).
//
//===----------------------------------------------------------------------===//

#include "../lib/Analysis/AnalysisImpl.h"

#include "revenant/Disasm/Disasm.h"
#include "revenant/Object/PeFile.h"
#include "revenant/Target/TargetHooks.h"

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

using namespace revenant;
using namespace revenant::analysis_detail;

namespace {

struct Env {
  std::shared_ptr<Image> Img;
  std::optional<Disasm> Dis;
  std::unique_ptr<TargetHooks> Hooks;
  Va Base{0x140000000};

  Env(std::vector<uint8_t> Text, uint64_t Vsize = 0) {
    Section S;
    S.Name = ".text";
    S.SecVa = Base;
    S.Vsize = Vsize ? Vsize : Text.size();
    S.Data = std::move(Text);
    S.Flags = SectionFlags::rx();
    Img = std::make_shared<Image>();
    Img->Base = Base;
    Img->Sections.push_back(std::move(S));
    llvm::Expected<Disasm> D = Disasm::create(*Img);
    EXPECT_TRUE(!!D);
    if (D)
      Dis = std::move(*D);
    else
      llvm::consumeError(D.takeError());
    Hooks = createX86TargetHooks();
  }

  Va at(uint64_t Off) const { return Va(Base.Value + Off); }
};

PeFile makePe(const std::shared_ptr<Image> &Img, Va Entry) {
  PeFile Pe;
  Pe.Img = Img;
  Pe.EntryPoint = Entry;
  return Pe;
}

Function fillersOnly(Va Start) {
  Function F;
  F.Start = Start;
  F.End = Start;
  for (int K = 0; K < 2; ++K) {
    Instr I;
    I.Addr = Va(Start.Value + (uint64_t)K);
    I.Size = 1;
    I.Inst.setOpcode(0);
    F.Instrs.push_back(I);
    F.End = F.End.wrappingAdd(1);
  }
  F.Chunks.push_back({Start, F.End});
  return F;
}

TEST(CandidatesTest, StrongStartsCoverEntryExportsTlsAndPrimaryPdata) {
  Env E({0xC3});
  PeFile Pe = makePe(E.Img, E.at(0));
  Export Fwd;
  Fwd.ExportVa = E.at(0);
  Fwd.Name = "fwd";
  Fwd.Forwarder = "other.fn";
  Pe.Exports.push_back(Fwd);
  Export Good;
  Good.ExportVa = E.at(0);
  Good.Name = "good";
  Pe.Exports.push_back(Good);
  Pe.TlsCallbacks.push_back(E.at(0));
  RuntimeFunction Primary;
  Primary.Begin = E.at(0);
  Primary.End = E.at(1);
  Primary.Unwind = E.at(0);
  Pe.Pdata.push_back(Primary);
  RuntimeFunction Chained = Primary;
  Chained.ChainedTo = E.at(0);
  Pe.Pdata.push_back(Chained);

  std::set<Va> Seeds = collectStrongStarts(Pe, *E.Img);
  ASSERT_EQ(Seeds.size(), 1u);
  EXPECT_TRUE(Seeds.count(E.at(0)));
}

TEST(CandidatesTest, InitialWeakCandidatesAreRelocTargets) {
  Env E({0xC3});
  PeFile Pe = makePe(E.Img, E.at(0));
  Pe.RelocTargets.push_back(E.at(0));
  std::set<Va> Weak = collectInitialWeakCandidates(Pe);
  ASSERT_EQ(Weak.size(), 1u);
  EXPECT_TRUE(Weak.count(E.at(0)));
}

TEST(CandidatesTest, WeakFunctionZeroAndFillerReject) {
  // All-zero chunk.
  Env E({0x00, 0x00});
  Function Zero;
  Zero.Start = E.at(0);
  Zero.End = E.at(2);
  Zero.Chunks.push_back({E.at(0), E.at(2)});
  Instr Z;
  Z.Addr = E.at(0);
  Z.Size = 2;
  Zero.Instrs.push_back(Z);
  EXPECT_TRUE(rejectWeakFunction(*E.Img, *E.Hooks, Zero));

  // Filler-only: int3 decodes as INT3, a NopOrInt3 member.
  Env F({0xCC, 0xCC});
  auto One = F.Dis->decodeOne(F.at(0));
  ASSERT_TRUE(One.has_value());
  Function Pad = fillersOnly(F.at(0));
  Pad.Instrs[0] = *One;
  Pad.Instrs[1] = *One;
  EXPECT_TRUE(F.Hooks->isNopOrInt3(*One));
  EXPECT_TRUE(rejectWeakFunction(*F.Img, *F.Hooks, Pad));

  // ret is neither all-zero nor filler-only.
  Env G({0xC3});
  auto Ret = G.Dis->decodeOne(G.at(0));
  ASSERT_TRUE(Ret.has_value());
  Function Real;
  Real.Start = G.at(0);
  Real.End = G.at(1);
  Real.Chunks.push_back({G.at(0), G.at(1)});
  Real.Instrs.push_back(*Ret);
  EXPECT_FALSE(rejectWeakFunction(*G.Img, *G.Hooks, Real));
}

} // end anonymous namespace
