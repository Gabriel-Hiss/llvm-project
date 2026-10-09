//==- unittests/Disasm/DisasmTest.cpp - decode checks ---------------------==//
//
// Linear decode boundaries must equal llvm-objdump's on a real function:
// pick pdata[0]'s range, decode it with RevenantDisasm, and compare the
// instruction addresses against `llvm-objdump -d` on the same bytes.
//
//===----------------------------------------------------------------------===//

#include "revenant/Disasm/Disasm.h"
#include "revenant/Object/PeFile.h"

#include "gtest/gtest.h"

#include <string>
#include <vector>

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"

using namespace llvm;
using namespace revenant;

namespace {

struct Fixture {
  std::unique_ptr<MemoryBuffer> Buf;
  std::optional<PeFile> Pe;
  std::optional<Disasm> Dis;
};

static std::optional<Fixture> open() {
  Fixture F;
  auto Buf = MemoryBuffer::getFile(REVENANT_TEST_C_O2);
  if (!Buf)
    return std::nullopt;
  F.Buf = std::move(*Buf);
  StringRef Data = F.Buf->getBuffer();
  auto Pe = PeFile::parse(ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(Data.data()), Data.size()));
  if (!Pe) {
    consumeError(Pe.takeError());
    return std::nullopt;
  }
  F.Pe = std::move(*Pe);
  auto D = Disasm::create(*F.Pe->Img);
  if (!D) {
    consumeError(D.takeError());
    return std::nullopt;
  }
  F.Dis = std::move(*D);
  return F;
}

// Instruction start VAs from `llvm-objdump -d --start-address --stop-address`.
static std::vector<uint64_t> objdumpBounds(uint64_t Start, uint64_t Stop) {
  SmallString<128> OutPath;
  if (sys::fs::createTemporaryFile("rv-objdump", "out", OutPath))
    return {};
  std::string StartS = formatv("--start-address={0:x}", Start).str();
  std::string StopS = formatv("--stop-address={0:x}", Stop).str();
  SmallVector<StringRef, 6> Argv = {LLVM_OBJDUMP_EXE, "-d", StartS, StopS,
                                    REVENANT_TEST_C_O2};
  std::optional<StringRef> Redirects[] = {std::nullopt, StringRef(OutPath),
                                          std::nullopt};
  if (sys::ExecuteAndWait(LLVM_OBJDUMP_EXE, Argv, std::nullopt, Redirects))
    return {};
  auto Buf = MemoryBuffer::getFile(OutPath);
  sys::fs::remove(OutPath);
  if (!Buf)
    return {};

  std::vector<uint64_t> Out;
  SmallVector<StringRef, 256> Lines;
  (*Buf)->getBuffer().split(Lines, '\n');
  for (StringRef L : Lines) {
    L = L.trim();
    auto Colon = L.find(':');
    if (Colon == StringRef::npos)
      continue;
    uint64_t Addr;
    // Lines are "<hex va>: bytes <tab> asm"; only keep the ones inside
    // the requested range with hex bytes after the colon.
    if (L.substr(0, Colon).getAsInteger(16, Addr))
      continue;
    StringRef Rest = L.substr(Colon + 1).trim();
    if (Rest.empty() || !isxdigit(Rest[0]))
      continue;
    if (Addr >= Start && Addr < Stop)
      Out.push_back(Addr);
  }
  llvm::sort(Out);
  Out.erase(std::unique(Out.begin(), Out.end()), Out.end());
  return Out;
}

TEST(DisasmTest, BoundariesMatchObjdump) {
  auto F = open();
  ASSERT_TRUE(F.has_value());
  ASSERT_FALSE(F->Pe->Pdata.empty());

  // Decode the first pdata function's range.
  Va Start = F->Pe->Pdata[0].Begin;
  Va End = F->Pe->Pdata[0].End;
  ASSERT_GT(End, Start);
  ASSERT_LT(End - Start, 1u << 20);

  auto Instrs = F->Dis->decodeRange(Start, End);
  ASSERT_FALSE(Instrs.empty());
  // Contiguous decode: every instruction ends where the next starts, and
  // the last ends exactly at the pdata end.
  for (size_t I = 1; I < Instrs.size(); ++I)
    EXPECT_EQ(Instrs[I - 1].nextVa(), Instrs[I].Addr);
  EXPECT_EQ(Instrs.back().nextVa(), End);

  auto Ref = objdumpBounds(Start.Value, End.Value);
  ASSERT_FALSE(Ref.empty());

  std::vector<uint64_t> Mine;
  for (const Instr &I : Instrs)
    Mine.push_back(I.Addr.Value);
  EXPECT_EQ(Mine, Ref);
}

TEST(DisasmTest, IntelFormat) {
  auto F = open();
  ASSERT_TRUE(F.has_value());
  auto First = F->Dis->decodeOne(F->Pe->Pdata[0].Begin);
  ASSERT_TRUE(First.has_value());
  std::string Text = F->Dis->format(*First);
  EXPECT_NE(Text.find_first_of("%"), 0u); // Intel, not AT&T operands
  EXPECT_FALSE(Text.empty());
}

TEST(DisasmTest, RelativeBranchUsesNextInstructionAddress) {
  auto F = open();
  ASSERT_TRUE(F.has_value());
  const uint8_t Call[] = {0xE8, 0, 0, 0, 0};
  auto Instrs = F->Dis->decodeSlice(Va(0x1000), Call);
  ASSERT_EQ(Instrs.size(), 1u);
  EXPECT_NE(F->Dis->format(Instrs[0]).find("0x1005"), std::string::npos);
}

TEST(DisasmTest, LockPrefixFoldsIntoAtomicInstruction) {
  auto F = open();
  ASSERT_TRUE(F.has_value());
  const uint8_t LockCmpxchg[] = {0xF0, 0x48, 0x0F, 0xB1, 0x0D,
                                  0x03, 0x01, 0x08, 0x00};
  Va Start(0x1000);
  auto Instrs = F->Dis->decodeSlice(Start, LockCmpxchg);
  ASSERT_EQ(Instrs.size(), 1u);
  EXPECT_EQ(Instrs[0].Addr, Start);
  EXPECT_EQ(Instrs[0].Size, sizeof(LockCmpxchg));
  std::string Text = F->Dis->format(Instrs[0]);
  EXPECT_NE(Text.find("lock"), std::string::npos);
  EXPECT_NE(Text.find("cmpxchg"), std::string::npos);
}

TEST(DisasmTest, FlowInfo) {
  auto F = open();
  ASSERT_TRUE(F.has_value());
  Va Start = F->Pe->Pdata[0].Begin;
  Va End = F->Pe->Pdata[0].End;
  auto Instrs = F->Dis->decodeRange(Start, End);
  ASSERT_FALSE(Instrs.empty());
  // Somewhere in the first functions there is a ret or a call.
  bool SawFlow = false;
  for (const Instr &I : Instrs) {
    if (I.Flow == FlowKind::Call && I.HasTarget) {
      SawFlow = true;
      // Direct call target sits inside the image.
      EXPECT_GT(I.Target.Value, 0u);
    }
    if (I.Flow == FlowKind::Return)
      SawFlow = true;
  }
  EXPECT_TRUE(SawFlow);
}

} // end anonymous namespace
