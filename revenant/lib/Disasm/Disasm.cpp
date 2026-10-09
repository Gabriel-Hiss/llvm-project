//==- revenant/lib/Disasm/Disasm.cpp - MC decoder -------------------------==//
//
// x86-64 decode + Intel-syntax print on LLVM's MC layer. Mirrors
// crates/disasm: one instruction at a VA, flow info via MCInstrAnalysis.
//
//===----------------------------------------------------------------------===//

#include "revenant/Disasm/Disasm.h"

#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrAnalysis.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

using namespace llvm;

namespace revenant {

Disasm::~Disasm() = default;
Disasm::Disasm(Disasm &&) = default;
Disasm &Disasm::operator=(Disasm &&) = default;

Expected<Disasm> Disasm::create(const Image &Img) {
  LLVMInitializeX86TargetInfo();
  LLVMInitializeX86TargetMC();
  LLVMInitializeX86Disassembler();
  LLVMInitializeX86AsmPrinter();

  Disasm D;
  D.Img = &Img;
  D.TT = std::make_unique<Triple>("x86_64-pc-windows-msvc");

  std::string Err;
  const Target *T = TargetRegistry::lookupTarget(*D.TT, Err);
  if (!T)
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             Twine("unknown target: ") + Err);
  D.MRI.reset(T->createMCRegInfo(*D.TT));
  if (!D.MRI)
    return createStringError(std::errc::invalid_argument, "no MCRegInfo");
  D.MAI.reset(T->createMCAsmInfo(*D.MRI, *D.TT, MCTargetOptions()));
  if (!D.MAI)
    return createStringError(std::errc::invalid_argument, "no MCAsmInfo");
  D.MII.reset(T->createMCInstrInfo());
  D.STI.reset(T->createMCSubtargetInfo(*D.TT, "", ""));
  if (!D.MII || !D.STI)
    return createStringError(std::errc::invalid_argument,
                             "no MCInstrInfo/MCSubtargetInfo");
  D.Ctx = std::make_unique<MCContext>(*D.TT, D.MAI.get(), D.MRI.get(),
                                      D.STI.get());
  D.Dis.reset(T->createMCDisassembler(*D.STI, *D.Ctx));
  if (!D.Dis)
    return createStringError(std::errc::invalid_argument,
                             "no MCDisassembler");
  D.MIA.reset(T->createMCInstrAnalysis(D.MII.get()));
  if (!D.MIA)
    return createStringError(std::errc::invalid_argument,
                             "no MCInstrAnalysis");
  // Syntax variant 1 is Intel on x86 (0 = AT&T, the asm default).
  D.IP.reset(T->createMCInstPrinter(*D.TT, 1, *D.MAI, *D.MII, *D.MRI));
  if (!D.IP)
    return createStringError(std::errc::invalid_argument,
                             "no Intel MCInstPrinter");
  // Immediates in hex, matching the Rust pipeline's IntelFormatter
  // options (uppercase hex, no zero displacements).
  D.IP->setPrintImmHex(true);
  D.IP->setPrintBranchImmAsAddress(true);
  return Expected<Disasm>(std::move(D));
}

// Decode `Bytes` at `Addr` into a full `Instr` (Bytes may extend past the
// instruction; the decoder reports Size).
static std::optional<Instr> decodeAt(const MCDisassembler &Dis,
                                     const MCInstrAnalysis &MIA,
                                     const MCSubtargetInfo &STI, Va Addr,
                                     ArrayRef<uint8_t> Bytes) {
  if (Bytes.empty())
    return std::nullopt;
  MCInst Inst;
  uint64_t Size = 0;
  auto Status = Dis.getInstruction(Inst, Size, Bytes, Addr.Value, nulls());
  if (Status != MCDisassembler::Success || Size == 0)
    return std::nullopt;

  Instr Out;
  Out.Addr = Addr;
  Out.Size = (uint32_t)Size;
  Out.Inst = Inst;

  if (MIA.isReturn(Inst))
    Out.Flow = FlowKind::Return;
  else if (MIA.isCall(Inst))
    Out.Flow = FlowKind::Call;
  else if (MIA.isUnconditionalBranch(Inst))
    Out.Flow = MIA.isIndirectBranch(Inst) ? FlowKind::IndirectBranch
                                        : FlowKind::Branch;
  else if (MIA.isConditionalBranch(Inst))
    Out.Flow = FlowKind::CondBranch;
  else if (MIA.isIndirectBranch(Inst))
    Out.Flow = FlowKind::IndirectBranch;
  else if (MIA.isBarrier(Inst))
    Out.Flow = FlowKind::Trap;

  uint64_t Target = 0;
  if ((MIA.isBranch(Inst) || MIA.isCall(Inst)) &&
      MIA.evaluateBranch(Inst, Addr.Value, Size, Target)) {
    Out.HasTarget = true;
    Out.Target = Va(Target);
  }
  if (auto MemAddr =
          MIA.evaluateMemoryOperandAddress(Inst, &STI, Addr.Value, Size)) {
    Out.HasMemTarget = true;
    Out.MemTarget = Va(*MemAddr);
  }
  return Out;
}

std::optional<Instr> Disasm::decodeOne(Va Addr) const {
  ArrayRef<uint8_t> Bytes = Img->dataAt(Addr, 16);
  return decodeAt(*Dis, *MIA, *STI, Addr, Bytes);
}

std::vector<Instr> Disasm::decodeRange(Va Start, Va End) const {
  std::vector<Instr> Out;
  if (End <= Start)
    return Out;
  size_t Len = (size_t)std::min<uint64_t>(End - Start, 1 << 20);
  ArrayRef<uint8_t> Bytes = Img->dataAt(Start, Len);
  Va Ip = Start;
  size_t Off = 0;
  while (Off < Bytes.size() && Ip < End) {
    auto One =
        decodeAt(*Dis, *MIA, *STI, Ip, Bytes.drop_front(Off));
    if (!One)
      break;
    Off += One->Size;
    Ip = Ip.wrappingAdd(One->Size);
    Out.push_back(std::move(*One));
  }
  return Out;
}

std::vector<Instr> Disasm::decodeSlice(Va Addr,
                                       ArrayRef<uint8_t> Bytes) const {
  std::vector<Instr> Out;
  Va Ip = Addr;
  size_t Off = 0;
  while (Off < Bytes.size()) {
    auto One =
        decodeAt(*Dis, *MIA, *STI, Ip, Bytes.drop_front(Off));
    if (!One)
      break;
    Off += One->Size;
    Ip = Ip.wrappingAdd(One->Size);
    Out.push_back(std::move(*One));
  }
  return Out;
}

std::string Disasm::format(const Instr &I, const SharedNames &Names) const {
  std::string Out;
  raw_string_ostream OS(Out);
  IP->printInst(&I.Inst, I.Addr.Value, "", *STI, OS);
  OS.flush();
  if (Names)
    return Names->decorateLine(I, Out, refs(I));
  return Out;
}

bool Disasm::isStore(const Instr &I) const {
  return MII->get(I.Inst.getOpcode()).mayStore();
}

std::vector<std::string> Disasm::formatAll(ArrayRef<Instr> Instrs,
                                           const SharedNames &Names) const {
  std::vector<std::string> Out;
  Out.reserve(Instrs.size());
  for (const Instr &I : Instrs)
    Out.push_back(format(I, Names));
  return Out;
}

std::vector<std::pair<Va, RefKind>> Disasm::refs(const Instr &I) const {
  std::vector<std::pair<Va, RefKind>> Out;
  if ((I.Flow == FlowKind::Call || I.Flow == FlowKind::Branch ||
       I.Flow == FlowKind::CondBranch ||
       I.Flow == FlowKind::IndirectBranch) &&
      I.HasTarget)
    Out.push_back(
        {I.Target,
         I.Flow == FlowKind::Call ? RefKind::Call : RefKind::Jump});
  if (I.HasMemTarget) {
    // call/jmp through memory land in RefKind::Call/Jump; lea is AddrOf.
    RefKind K = RefKind::Data;
    StringRef Mn = MII->getName(I.Inst.getOpcode());
    if (I.Flow == FlowKind::Call)
      K = RefKind::Call;
    else if (Mn.starts_with("LEA"))
      K = RefKind::AddrOf;
    else if (I.Flow == FlowKind::IndirectBranch ||
             I.Flow == FlowKind::Branch)
      K = RefKind::Jump;
    Out.push_back({I.MemTarget, K});
  }
  return Out;
}

} // end namespace revenant
