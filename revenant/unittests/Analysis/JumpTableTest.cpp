//==- unittests/Analysis/JumpTableTest.cpp - jump-table recovery -----------==//
//
// Exercise the target operand adapter and a region-bounded table recovery
// regression from crates/analysis/src/jump_table.rs.
//
//===----------------------------------------------------------------------===//

#include "revenant/Analysis/JumpTable.h"
#include "revenant/Disasm/Disasm.h"

#include "gtest/gtest.h"
#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <vector>

#include "llvm/Support/Endian.h"
using namespace revenant;

namespace {

class Hooks final : public TargetHooks {
public:
  llvm::StringRef mnemonic(const Instr &) const override { return {}; }
  bool isFiller(const Instr &) const override { return false; }
  TrapKind trapKind(const Instr &) const override { return TrapKind::NotTrap; }
  bool isNopOrInt3(const Instr &) const override { return false; }
  bool isOpaqueTailJmp(const Image &, const Instr &) const override {
    return false;
  }
  std::optional<int64_t> stackDelta(const Instr &) const override {
    return std::nullopt;
  }
  StackSlots stackSlots(const Instr &) const override { return {}; }
  FpEstablish fpEstablish(const Instr &) const override { return {}; }
  bool restoresSpFromFp(const Instr &) const override { return false; }
  std::optional<int64_t> movEaxImm(const Instr &) const override {
    return std::nullopt;
  }
  bool writesSpDynamically(const Instr &) const override { return false; }
  const JtInsnOps *jumpTableInsnOps() const override {
    return getX86JumpTableInsnOps();
  }
  std::optional<JumpTableInfo>
  matchJumpTable(const Image &, llvm::ArrayRef<std::pair<Va, Va>>,
                 llvm::ArrayRef<BasicBlock>, llvm::ArrayRef<Instr>, size_t,
                 const std::vector<Va> &, const std::vector<Va> &,
                 const std::map<Va, uint64_t> &) const override {
    return std::nullopt;
  }
};

static std::vector<Instr> decode(const Image &Img, Va Start, Va End) {
  auto D = Disasm::create(Img);
  EXPECT_TRUE((bool)D);
  if (!D)
    return {};
  std::vector<Instr> Out;
  for (Va P = Start; P < End;) {
    auto I = D->decodeOne(P);
    if (!I)
      break;
    P = I->nextVa();
    Out.push_back(std::move(*I));
  }
  return Out;
}

class JumpTableOperandTest : public testing::Test {
protected:
  Image Img;
  std::optional<Disasm> Decoder;

  void SetUp() override {
    Img.Base = Va(0x140000000);
    Section Text;
    Text.Name = ".text";
    Text.SecVa = Img.Base;
    Text.Vsize = 8;
    Text.Flags = SectionFlags::rx();
    Text.Data = {0x48, 0x63, 0x04, 0x81, 0x83, 0xf8, 0x07, 0x77};
    Img.Sections.push_back(std::move(Text));
    auto D = Disasm::create(Img);
    ASSERT_TRUE((bool)D);
    Decoder = std::move(*D);
  }
};

TEST_F(JumpTableOperandTest, RecognizesSignedRelativeTableLoad) {
  auto I = Decoder->decodeOne(Img.Base);
  ASSERT_TRUE(I);
  JtInsn O = getX86JumpTableInsnOps()->operands(*I);
  EXPECT_EQ(O.Mn, JtMnemonic::Movsxd);
  EXPECT_EQ(O.Op0, JtOpKind::Reg);
  EXPECT_EQ(O.Op1, JtOpKind::Mem);
  EXPECT_NE(O.MemIndex, 0u);
  EXPECT_EQ(O.MemScale, 4u);
  EXPECT_EQ(O.MemSize, 4u);
}

TEST_F(JumpTableOperandTest, KeepsSignExtendedEightBitGuardImmediate) {
  auto I = Decoder->decodeOne(Img.Base.wrappingAdd(4));
  ASSERT_TRUE(I);
  JtInsn O = getX86JumpTableInsnOps()->operands(*I);
  EXPECT_EQ(O.Mn, JtMnemonic::Cmp);
  EXPECT_EQ(O.Op0, JtOpKind::Reg);
  EXPECT_EQ(O.Op1, JtOpKind::Imm);
  EXPECT_EQ(O.Imm, 7);
  EXPECT_EQ(O.ImmForm, JtImmForm::I8Wide);
}

TEST(JumpTableRecoveryTest, UnrelatedGuardUsesCodeRangeBound) {
  std::vector<uint8_t> Bytes(0x300, 0xcc);
  const uint8_t Code[] = {
      0x49, 0x81, 0xf8, 0xff, 0xff, 0xff, 0x7f, // cmp r8, 0x7fffffff
      0x77, 0x17,                               // ja default
      0x48, 0x8d, 0x0d, 0xf0, 0x00, 0x00, 0x00, // lea rcx, [0x1100]
      0x48, 0x63, 0x04, 0x81,                   // movsxd rax, [rcx+rax*4]
      0x48, 0x01, 0xc8,                         // add rax, rcx
      0xff, 0xe0};                              // jmp rax
  std::copy(std::begin(Code), std::end(Code), Bytes.begin());
  Bytes[0x20] = 0xc3;
  Bytes[0x21] = 0xc3;
  const int32_t Offsets[] = {-0xe0, -0xdf, 0x100};
  for (size_t I = 0; I < std::size(Offsets); ++I)
    llvm::support::endian::write32le(Bytes.data() + 0x100 + 4 * I,
                                     (uint32_t)Offsets[I]);
  Image Img;
  Img.Base = Va(0x1000);
  Section Text;
  Text.Name = ".text";
  Text.SecVa = Img.Base;
  Text.Vsize = Bytes.size();
  Text.Data = std::move(Bytes);
  Text.Flags = SectionFlags::rx();
  Img.Sections.push_back(std::move(Text));

  std::vector<Instr> Instrs = decode(Img, Va(0x1000), Va(0x1019));
  ASSERT_EQ(Instrs.size(), 6u);
  std::vector<BasicBlock> Blocks(2);
  Blocks[0].Start = Va(0x1000);
  Blocks[0].End = Va(0x1009);
  Blocks[0].InstrsBegin = 0;
  Blocks[0].InstrsEnd = 2;
  Blocks[0].Edges = {{Va(0x1020), EdgeKind::CondTaken},
                     {Va(0x1009), EdgeKind::CondNotTaken}};
  Blocks[0].EndKind = BlockEnd::Branch;
  Blocks[1].Start = Va(0x1009);
  Blocks[1].End = Va(0x1019);
  Blocks[1].InstrsBegin = 2;
  Blocks[1].InstrsEnd = Instrs.size();
  std::set<Va> Refs;
  std::map<Va, uint64_t> Limits;
  std::pair<Va, Va> Range{Va(0x1000), Va(0x1022)};
  JumpTableCtx Ctx{Img, llvm::ArrayRef<std::pair<Va, Va>>(Range), Refs, Refs,
                   Limits};
  Hooks H;
  const JtInsnOps *Ops = H.jumpTableInsnOps();
  JtInsn TableLoad = Ops->operands(Instrs[3]);
  EXPECT_EQ(TableLoad.Mn, JtMnemonic::Movsxd);
  EXPECT_EQ(TableLoad.MemScale, 4u);
  EXPECT_EQ(TableLoad.MemDisp, 0u);
  EXPECT_EQ(Ops->operands(Instrs[2]).MemDisp, 0x1100u);
  EXPECT_EQ(Instrs[5].Flow, FlowKind::IndirectBranch);
  EXPECT_TRUE(jumpTableDataSpan(Ctx, H, Instrs, Blocks, 1));
  auto Result = resolveJumpTable(Ctx, H, Blocks, Instrs, 1);
  ASSERT_TRUE(Result);
  EXPECT_FALSE(Result->Guard);
  ASSERT_EQ(Result->Targets.size(), 2u);
  EXPECT_EQ(Result->Targets[0].first, Va(0x1020));
  EXPECT_EQ(Result->Targets[1].first, Va(0x1021));
}

TEST(JumpTableRecoveryTest, MsvcImageBaseTableUsesRvaEntries) {
  std::vector<uint8_t> Bytes(0x300, 0xcc);
  const uint8_t Code[] = {0x48, 0x8d, 0x15, 0xf9, 0xff, 0xff, 0xff,
                          0x8b, 0x8c, 0x82, 0x00, 0x02, 0x00, 0x00,
                          0x48, 0x01, 0xd1, 0xff, 0xe1};
  std::copy(std::begin(Code), std::end(Code), Bytes.begin());
  llvm::support::endian::write32le(Bytes.data() + 0x200, 0x40);
  llvm::support::endian::write32le(Bytes.data() + 0x204, 0x41);
  llvm::support::endian::write32le(Bytes.data() + 0x208, 0x1000);
  Bytes[0x40] = 0xc3;
  Bytes[0x41] = 0xc3;
  Image Img;
  Img.Base = Va(0x140000000);
  Section Text;
  Text.Name = ".text";
  Text.SecVa = Img.Base;
  Text.Vsize = Bytes.size();
  Text.Data = std::move(Bytes);
  Text.Flags = SectionFlags::rx();
  Img.Sections.push_back(std::move(Text));

  std::vector<Instr> Instrs =
      decode(Img, Img.Base, Img.Base.wrappingAdd(sizeof(Code)));
  ASSERT_EQ(Instrs.size(), 4u);
  BasicBlock Dispatch;
  Dispatch.Start = Img.Base;
  Dispatch.End = Img.Base.wrappingAdd(sizeof(Code));
  Dispatch.InstrsBegin = 0;
  Dispatch.InstrsEnd = Instrs.size();
  Dispatch.EndKind = BlockEnd::UnresolvedIndirect;
  std::vector<BasicBlock> Blocks{Dispatch};
  std::set<Va> Refs;
  std::map<Va, uint64_t> Limits;
  std::pair<Va, Va> Range{Img.Base, Img.Base.wrappingAdd(0x100)};
  JumpTableCtx Ctx{Img, llvm::ArrayRef<std::pair<Va, Va>>(Range), Refs, Refs,
                   Limits};
  Hooks H;
  auto Result = resolveJumpTable(Ctx, H, Blocks, Instrs, 0);
  ASSERT_TRUE(Result);
  EXPECT_EQ(Result->Table, Img.Base.wrappingAdd(0x200));
  ASSERT_EQ(Result->Targets.size(), 2u);
  EXPECT_EQ(Result->Targets[0].first, Img.Base.wrappingAdd(0x40));
  EXPECT_EQ(Result->Targets[1].first, Img.Base.wrappingAdd(0x41));
}

TEST(JumpTableRecoveryTest, ClangTwoLevelByteTableResolvesCases) {
  std::vector<uint8_t> Bytes(0x500, 0xcc);
  const uint8_t Code[] = {
      0x48, 0x8d, 0x0d, 0xf9, 0x02, 0x00, 0x00, // lea rcx,[0x1300]
      0x0f, 0xb6, 0x84, 0x01, 0x00, 0xff, 0xff, 0xff, // movzx eax,[rcx+rax-100h]
      0x48, 0x63, 0x04, 0x81, // movsxd rax,[rcx+rax*4]
      0x48, 0x01, 0xc8,       // add rax,rcx
      0xff, 0xe0};            // jmp rax
  std::copy(std::begin(Code), std::end(Code), Bytes.begin());
  Bytes[0x40] = 0xc3;
  Bytes[0x41] = 0xc3;
  Bytes[0x200] = 0;
  Bytes[0x201] = 1;
  Bytes[0x202] = 2;
  llvm::support::endian::write32le(Bytes.data() + 0x300, (uint32_t)-0x2c0);
  llvm::support::endian::write32le(Bytes.data() + 0x304, (uint32_t)-0x2bf);
  llvm::support::endian::write32le(Bytes.data() + 0x308, 0x1000);
  Image Img;
  Img.Base = Va(0x1000);
  Section Text;
  Text.Name = ".text";
  Text.SecVa = Img.Base;
  Text.Vsize = Bytes.size();
  Text.Data = std::move(Bytes);
  Text.Flags = SectionFlags::rx();
  Img.Sections.push_back(std::move(Text));
  std::vector<Instr> Instrs =
      decode(Img, Img.Base, Img.Base.wrappingAdd(sizeof(Code)));
  ASSERT_EQ(Instrs.size(), 5u);
  BasicBlock Dispatch;
  Dispatch.Start = Img.Base;
  Dispatch.End = Img.Base.wrappingAdd(sizeof(Code));
  Dispatch.InstrsBegin = 0;
  Dispatch.InstrsEnd = Instrs.size();
  Dispatch.EndKind = BlockEnd::UnresolvedIndirect;
  std::vector<BasicBlock> Blocks{Dispatch};
  std::set<Va> Refs;
  std::map<Va, uint64_t> Limits;
  std::pair<Va, Va> Range{Img.Base, Img.Base.wrappingAdd(0x100)};
  JumpTableCtx Ctx{Img, llvm::ArrayRef<std::pair<Va, Va>>(Range), Refs, Refs,
                   Limits};
  Hooks H;
  auto Result = resolveJumpTable(Ctx, H, Blocks, Instrs, 0);
  ASSERT_TRUE(Result);
  EXPECT_EQ(Result->Table, Img.Base.wrappingAdd(0x300));
  EXPECT_EQ(Result->EnumBase, Img.Base.wrappingAdd(0x200));
  ASSERT_EQ(Result->Targets.size(), 2u);
  EXPECT_EQ(Result->Targets[0].first, Img.Base.wrappingAdd(0x40));
  EXPECT_EQ(Result->Targets[1].first, Img.Base.wrappingAdd(0x41));
}

TEST(JumpTableRecoveryTest, GccMaskedDispatchUsesTrampolinePredecessor) {
  std::vector<uint8_t> Bytes(0x600, 0xcc);
  const uint8_t Setup[] = {
      0x48, 0x8b, 0x85, 0x60, 0x0c, 0x00, 0x00,
      0x48, 0x8d, 0x50, 0x04,
      0x48, 0x89, 0x95, 0x60, 0x0c, 0x00, 0x00,
      0x8b, 0x00,
      0x89, 0x85, 0x58, 0x0c, 0x00, 0x00,
      0x8b, 0x85, 0x58, 0x0c, 0x00, 0x00,
      0x83, 0xe0, 0x7f,
      0x89, 0xc0,
      0x48, 0x8d, 0x14, 0xc5, 0x00, 0x00, 0x00, 0x00,
      0x48, 0x8d, 0x05, 0xcc, 0x04, 0x00, 0x00,
      0x48, 0x8b, 0x04, 0x02,
      0xeb, 0x02};
  std::copy(std::begin(Setup), std::end(Setup), Bytes.begin());
  Bytes[sizeof(Setup)] = 0xff;
  Bytes[sizeof(Setup) + 1] = 0xe0;
  llvm::support::endian::write64le(Bytes.data() + 0x500, 0x1200);
  llvm::support::endian::write64le(Bytes.data() + 0x508, 0x1210);
  Bytes[0x200] = 0xc3;
  Bytes[0x210] = 0xc3;
  Image Img;
  Img.Base = Va(0x1000);
  Section Text;
  Text.Name = ".text";
  Text.SecVa = Img.Base;
  Text.Vsize = Bytes.size();
  Text.Data = std::move(Bytes);
  Text.Flags = SectionFlags::rx();
  Img.Sections.push_back(std::move(Text));

  std::vector<Instr> Instrs =
      decode(Img, Va(0x1000), Va(0x1000 + sizeof(Setup) + 2));
  ASSERT_GE(Instrs.size(), 3u);
  const size_t Tail = Instrs.size() - 1;
  std::vector<BasicBlock> Blocks(2);
  Blocks[0].Start = Va(0x1000);
  Blocks[0].End = Instrs[Tail].Addr;
  Blocks[0].InstrsBegin = 0;
  Blocks[0].InstrsEnd = Tail;
  Blocks[0].Edges = {{Instrs[Tail].Addr, EdgeKind::Jump}};
  Blocks[0].EndKind = BlockEnd::Branch;
  Blocks[1].Start = Instrs[Tail].Addr;
  Blocks[1].End = Instrs[Tail].nextVa();
  Blocks[1].InstrsBegin = Tail;
  Blocks[1].InstrsEnd = Instrs.size();
  Blocks[1].EndKind = BlockEnd::UnresolvedIndirect;
  std::set<Va> Refs;
  std::map<Va, uint64_t> Limits;
  std::pair<Va, Va> Range{Va(0x1000), Va(0x1600)};
  JumpTableCtx Ctx{Img, llvm::ArrayRef<std::pair<Va, Va>>(Range), Refs, Refs,
                   Limits};
  Hooks H;
  auto Result = resolveJumpTable(Ctx, H, Blocks, Instrs, 1);
  ASSERT_TRUE(Result);
  EXPECT_FALSE(Result->Guard);
  EXPECT_EQ(Result->Table, Va(0x1500));
  ASSERT_EQ(Result->Targets.size(), 2u);
  EXPECT_EQ(Result->Targets[0].first, Va(0x1200));
  EXPECT_EQ(Result->Targets[1].first, Va(0x1210));
}

TEST(JumpTableImmediateTest, ReadsSubByteBoundAsUnsigned) {
  Image Img;
  Img.Base = Va(0x1000);
  Section Text;
  Text.Name = ".text";
  Text.SecVa = Img.Base;
  Text.Vsize = 5;
  Text.Data = {0x80, 0xea, 0xb7, 0x77, 0x02};
  Text.Flags = SectionFlags::rx();
  Img.Sections.push_back(std::move(Text));
  auto D = Disasm::create(Img);
  ASSERT_TRUE((bool)D);
  auto Sub = D->decodeOne(Img.Base);
  ASSERT_TRUE(Sub);
  JtInsn I = getX86JumpTableInsnOps()->operands(*Sub);
  EXPECT_EQ(I.Mn, JtMnemonic::Sub);
  EXPECT_EQ(I.ImmForm, JtImmForm::I8);
  EXPECT_EQ((uint8_t)I.Imm, 0xb7);
  auto Branch = D->decodeOne(Img.Base.wrappingAdd(3));
  ASSERT_TRUE(Branch);
  EXPECT_EQ(getX86JumpTableInsnOps()->operands(*Branch).Cond, JtCond::A);
}

// Dispatch bytes from lz4-clang-o1's XXH32_finalize (0x1adfe): a 16-case
// clang switch whose case bodies live on both sides of the dispatch, with
// the case table parked in .rdata next to the next function's table.
// The dispatch block is chained into a different function (a tail `jmp`
// absorbed it), so the owning function's code region does not contain it.
TEST(JumpTableRecoveryTest, ChainedSwitchTailIgnoresForeignCodeRange) {
  std::vector<uint8_t> Text(0x2000, 0xcc);
  const uint8_t Dispatch[] = {
      0x41, 0x83, 0xe0, 0x0f,                   // and r8d, 0xf
      0x48, 0x8d, 0x05, 0xf5, 0x6f, 0x00, 0x00, // lea rax, [0x8000]
      0x4e, 0x63, 0x04, 0x80,                   // movsxd r8, [rax+r8*4]
      0x49, 0x01, 0xc8,                         // add r8, rax
      0x41, 0xff, 0xe0};                        // jmp r8
  std::copy(std::begin(Dispatch), std::end(Dispatch), Text.begin());
  // Ten case bodies below the dispatch and six above it, mirroring the
  // real switch's layout.
  const Va Cases[] = {Va(0x1400), Va(0x1500), Va(0x1600), Va(0x1700),
                      Va(0x1800), Va(0x1900), Va(0x1a00), Va(0x1b00),
                      Va(0x1c00), Va(0x1d00), Va(0x2000), Va(0x2100),
                      Va(0x2200), Va(0x2300), Va(0x2400), Va(0x2500)};
  for (Va C : Cases)
    Text[C.Value - 0x1000] = 0xc3;

  std::vector<uint8_t> Rdata(0x100, 0xcc);
  for (size_t I = 0; I < std::size(Cases); ++I)
    llvm::support::endian::write32le(Rdata.data() + 4 * I,
                                     (uint32_t)(Cases[I].Value - 0x8000));
  Image Img;
  Img.Base = Va(0x1000);
  Section Txt;
  Txt.Name = ".text";
  Txt.SecVa = Img.Base;
  Txt.Vsize = Text.size();
  Txt.Data = std::move(Text);
  Txt.Flags = SectionFlags::rx();
  Img.Sections.push_back(std::move(Txt));
  Section RdataSec;
  RdataSec.Name = ".rdata";
  RdataSec.SecVa = Va(0x8000);
  RdataSec.Vsize = Rdata.size();
  RdataSec.Data = std::move(Rdata);
  RdataSec.Flags = SectionFlags::r();
  Img.Sections.push_back(std::move(RdataSec));

  const Va DispatchIp = Img.Base;
  std::vector<Instr> Instrs = decode(Img, DispatchIp, Va(0x1015));
  ASSERT_EQ(Instrs.size(), 5u);
  EXPECT_EQ(Instrs.back().Flow, FlowKind::IndirectBranch);
  BasicBlock Block;
  Block.Start = DispatchIp;
  Block.End = Instrs.back().nextVa();
  Block.InstrsBegin = 0;
  Block.InstrsEnd = Instrs.size();
  Block.EndKind = BlockEnd::UnresolvedIndirect;
  std::vector<BasicBlock> Blocks{Block};
  std::set<Va> Refs;
  std::map<Va, uint64_t> Limits;

  // The owning function's region ends long before the dispatch block, so
  // every case body of this table sits outside it.
  std::pair<Va, Va> ForeignRange{Va(0x3000), Va(0x3200)};
  JumpTableCtx Ctx{Img, llvm::ArrayRef<std::pair<Va, Va>>(ForeignRange), Refs,
                   Refs, Limits};
  Hooks H;
  auto Result = resolveJumpTable(Ctx, H, Blocks, Instrs, 0);
  ASSERT_TRUE(Result);
  EXPECT_EQ(Result->Table, Va(0x8000));
  ASSERT_EQ(Result->Targets.size(), 16u);
  for (Va C : Cases)
    EXPECT_TRUE(llvm::any_of(Result->Targets,
                             [&](const std::pair<Va, std::vector<int64_t>> &T) {
                               return T.first == C;
                             }))
        << "missing case at " << C.Value;

  // With the dispatch inside the region the old bound still applies: the
  // walk stops at the first case body that lies outside it, so the six
  // low-address cases past the tenth entry are never reached.
  std::pair<Va, Va> OwnRange{Va(0x1000), Va(0x1e00)};
  JumpTableCtx Own{Img, llvm::ArrayRef<std::pair<Va, Va>>(OwnRange), Refs,
                   Refs, Limits};
  auto Bounded = resolveJumpTable(Own, H, Blocks, Instrs, 0);
  ASSERT_TRUE(Bounded);
  ASSERT_EQ(Bounded->Targets.size(), 10u);
  EXPECT_EQ(Bounded->Targets.front().first, Va(0x1400));
  EXPECT_EQ(Bounded->Targets.back().first, Va(0x1d00));
}

} // namespace
