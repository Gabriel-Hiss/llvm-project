//===- unittests/Lift/LiftSemanticsTest.cpp - lifted vs native results -------===//
//
// Differential test of the instruction semantics. Each case is a few x86
// instructions with chosen register, stack and vector inputs. The same
// bytes run twice: natively in this process, and lifted to IR with the
// function lifter and compiled by the JIT. Both runs store the registers,
// the five tracked flags, eight stack qwords and the vector registers into
// a buffer; the test compares the buffers under a per-case mask that leaves
// out architecturally undefined results.
//
// The bytes come from llvm-mc (`-show-encoding`), so cases are plain Intel
// assembly. Every case runs under two initial flag states so a flag that
// should be preserved is distinguishable from one that is cleared.
//
//===----------------------------------------------------------------------===//

#include "LiftSemanticsCase.h"
#include "LiftTestUtil.h"
#include "revenant/Disasm/Disasm.h"
#include "revenant/Lift/LiftFunction.h"

#include "X86MCTargetDesc.h"

#include "gtest/gtest.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Memory.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/Support/Program.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/Support/TargetSelect.h"

namespace {

using namespace revenant;
using namespace lifttest;

// ---- assembling ------------------------------------------------------------

static std::string hexQ(uint64_t V) { return "0x" + llvm::utohexstr(V); }

static std::string snippet(const Case &C, bool FlagVariantB) {
  std::string S = ".intel_syntax noprefix\n";
  auto L = [&](const std::string &Line) { S += Line + "\n"; };
  for (const char *R : {"rbx", "rbp", "rsi", "rdi", "r12", "r13", "r14", "r15"})
    L(std::string("push ") + R);
  L("sub rsp, 0x408"); // 16-byte aligned; holds eight 64-byte vectors
  L("mov r15, rcx");
  // xmm6/xmm7 are callee-saved in the Windows x64 ABI.
  L("movdqu xmmword ptr [rsp+0x300], xmm6");
  L("movdqu xmmword ptr [rsp+0x310], xmm7");
  // A defined x87 state per case: the native side runs many snippets in one
  // process and the FPU stack is not otherwise reset between them.
  L("fninit");
  for (size_t K = 0; K < C.Stack.size(); ++K) {
    L("mov rax, " + hexQ(C.Stack[K]));
    L("mov qword ptr [rsp+" + hexQ(0x20 + 8 * K) + "], rax");
  }
  if (C.VecUsed) {
    unsigned Bytes = C.Zmm ? 64 : C.Ymm ? 32 : 16;
    const char *Width = C.Zmm ? "zmmword" : C.Ymm ? "ymmword" : "xmmword";
    for (size_t N = 0; N < VecRegs; ++N) {
      for (unsigned Q = 0; Q < Bytes / 8; ++Q) {
        uint64_t V = N < C.Vec.size() && C.Given[N]
                         ? (Q < 4 ? C.Vec[N][Q] : C.VecHi[N][Q - 4])
                         : 0x0101010101010101ull * (N * 4 + Q + 1);
        L("mov rax, " + hexQ(V));
        L("mov qword ptr [rsp+" + hexQ(0x80 + Bytes * N + 8 * Q) + "], rax");
      }
      L(std::string(C.Zmm ? "vmovdqu64 zmm" : C.Ymm ? "vmovdqu ymm"
                                                     : "movdqu xmm") +
        std::to_string(N) + ", " + Width + " ptr [rsp+" +
        hexQ(0x80 + Bytes * N) + "]");
    }
  }
  uint64_t Nonce = 0x1111111111111111ull;
  for (unsigned K = 0; K < 10; ++K) {
    uint64_t V = Nonce * (K + 1);
    for (auto &[Rn, Rv] : C.Regs)
      if (Rn == RegName[K])
        V = Rv;
    L(std::string("mov ") + RegName[K] + ", " + hexQ(V));
  }
  // Initial flags: A = CF,SF,PF set; B = ZF,PF set.
  L(FlagVariantB ? "mov rbp, 1" : "mov rbp, 0");
  L("sub rbp, 1");
  S += C.Asm + "\n";
  for (unsigned K = 0; K < 10; ++K)
    L("mov qword ptr [r15+" + std::to_string(8 * K) + "], " + RegName[K]);
  const char *Cc[5] = {"setb", "sete", "sets", "seto", "setp"};
  for (unsigned K = 0; K < 5; ++K) {
    L(std::string(Cc[K]) + " al");
    L("movzx eax, al");
    L("mov qword ptr [r15+" + std::to_string(8 * (SlotFlags + K)) + "], rax");
  }
  for (unsigned K = 0; K < StackQwords; ++K) {
    L("mov rax, qword ptr [rsp+" + hexQ(0x20 + 8 * K) + "]");
    L("mov qword ptr [r15+" + std::to_string(8 * (SlotStack + K)) + "], rax");
  }
  for (unsigned N = 0; C.VecUsed && N < VecRegs; ++N) {
    unsigned Off = 8 * (SlotVec + N * VecQwords);
    if (C.Zmm) {
      // A zmm needs 64 bytes; spill it to the frame and copy out.
      L("vmovdqu64 zmmword ptr [rsp+0x340], zmm" + std::to_string(N));
      for (unsigned Q = 0; Q < 4; ++Q) {
        L("mov rax, qword ptr [rsp+" + hexQ(0x340 + 8 * Q) + "]");
        L("mov qword ptr [r15+" + std::to_string(Off + 8 * Q) + "], rax");
      }
    } else if (C.Ymm)
      L("vmovdqu ymmword ptr [r15+" + std::to_string(Off) + "], ymm" +
        std::to_string(N));
    else
      L("movdqu xmmword ptr [r15+" + std::to_string(Off) + "], xmm" +
        std::to_string(N));
  }
  L("movdqu xmm6, xmmword ptr [rsp+0x300]");
  L("movdqu xmm7, xmmword ptr [rsp+0x310]");
  L("add rsp, 0x408");
  for (const char *R : {"r15", "r14", "r13", "r12", "rdi", "rsi", "rbp", "rbx"})
    L(std::string("pop ") + R);
  L("ret");
  return S;
}

static bool assemble(const std::string &Text, std::vector<uint8_t> &Out,
                     std::string &Err) {
  llvm::SmallString<128> In, OutPath;
  int Fd;
  if (llvm::sys::fs::createTemporaryFile("rvsem", "s", Fd, In)) {
    Err = "temp file";
    return false;
  }
  llvm::raw_fd_ostream(Fd, true) << Text;
  llvm::sys::fs::createTemporaryFile("rvsem", "txt", OutPath);
  llvm::StringRef Args[] = {REVENANT_TEST_LLVM_MC, "-triple=x86_64-pc-windows-msvc",
                            "-show-encoding", In};
  std::optional<llvm::StringRef> Redirects[] = {std::nullopt,
                                                llvm::StringRef(OutPath),
                                                llvm::StringRef(OutPath)};
  int Rc = llvm::sys::ExecuteAndWait(REVENANT_TEST_LLVM_MC, Args, std::nullopt,
                                     Redirects);
  auto Buf = llvm::MemoryBuffer::getFile(OutPath);
  llvm::sys::fs::remove(In);
  llvm::sys::fs::remove(OutPath);
  if (!Buf) {
    Err = "no llvm-mc output";
    return false;
  }
  llvm::StringRef Txt = (*Buf)->getBuffer();
  if (Rc != 0) {
    Err = Txt.str();
    return false;
  }
  Out.clear();
  for (llvm::StringRef Line : llvm::split(Txt, '\n')) {
    size_t P = Line.find("encoding: [");
    if (P == llvm::StringRef::npos)
      continue;
    llvm::StringRef Bytes = Line.substr(P + 11);
    Bytes = Bytes.take_until([](char C) { return C == ']'; });
    llvm::SmallVector<llvm::StringRef, 16> Parts;
    Bytes.split(Parts, ',');
    for (llvm::StringRef B : Parts) {
      unsigned V;
      if (B.trim().drop_front(2).getAsInteger(16, V)) {
        Err = "bad encoding byte";
        return false;
      }
      Out.push_back((uint8_t)V);
    }
  }
  return true;
}

// ---- running ---------------------------------------------------------------

static std::vector<uint64_t> runNative(llvm::ArrayRef<uint8_t> Code) {
  std::error_code EC;
  llvm::sys::MemoryBlock MB = llvm::sys::Memory::allocateMappedMemory(
      Code.size(), nullptr,
      llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE, EC);
  std::memcpy(MB.base(), Code.data(), Code.size());
  llvm::sys::Memory::protectMappedMemory(
      MB, llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_EXEC);
  llvm::sys::Memory::InvalidateInstructionCache(MB.base(), MB.allocatedSize());
  std::vector<uint64_t> Out(BufferQwords, 0);
  reinterpret_cast<void (*)(uint64_t *)>(MB.base())(Out.data());
  llvm::sys::Memory::releaseMappedMemory(MB);
  return Out;
}

static bool runLifted(llvm::ArrayRef<uint8_t> Code, std::vector<uint64_t> &Out,
                      std::string &Err) {
  auto Img = std::make_shared<Image>();
  Img->Base = Va(0x140000000);
  Section Sec;
  Sec.Name = ".text";
  Sec.SecVa = Va(0x140001000);
  Sec.Vsize = Code.size();
  Sec.Data.assign(Code.begin(), Code.end());
  Sec.Flags = SectionFlags::rx();
  Img->Sections.push_back(Sec);
  auto D = Disasm::create(*Img);
  if (!D) {
    Err = llvm::toString(D.takeError());
    return false;
  }
  if (std::getenv("REVENANT_SEM_VERBOSE"))
    llvm::errs() << "  decode\n";
  std::vector<Instr> Block = D->decodeSlice(Sec.SecVa, Code);
  if (std::getenv("REVENANT_SEM_VERBOSE"))
    llvm::errs() << "  decoded " << Block.size() << "\n";
  if (Block.empty() || Block.back().nextVa() != Sec.SecVa + Code.size()) {
    Err = "decode did not cover the snippet";
    return false;
  }
  Program P = lifttest::emptyProgram(Img);
  FuncRecord R = lifttest::singleBlockFunc(P, Block);
  lift::Signatures Sigs;
  lift::Signature Sig;
  Sig.Params.push_back(
      {lift::ParamLoc::reg(llvm::X86::RCX), 64, false, std::nullopt});
  Sigs.Funcs[R.Start] = Sig;

  auto Ctx = std::make_unique<llvm::LLVMContext>();
  auto M = std::make_unique<llvm::Module>("sem", *Ctx);
  auto Lifted = lift::liftFunction(*M, P, R, *D, &Sigs, nullptr, false);
  if (!Lifted) {
    Err = llvm::toString(Lifted.takeError());
    return false;
  }
  std::string Name = Lifted->Fn->getName().str();
  // COFF constant-pool symbols (__ymm@...) do not link in the JIT, so compile
  // the lifted module as ELF for the host CPU. One JIT serves every case,
  // each in its own dylib, because a JIT per case exhausts the host.
  static llvm::Triple JitTriple("x86_64-unknown-linux-gnu");
  static std::unique_ptr<llvm::orc::LLJIT> Jit;
  static unsigned Serial = 0;
  if (!Jit) {
    llvm::orc::JITTargetMachineBuilder JTMB(JitTriple);
    JTMB.setCPU(llvm::sys::getHostCPUName().str());
    for (auto &F : llvm::sys::getHostCPUFeatures())
      JTMB.addFeatures(
          {(std::string(F.second ? "+" : "-") + F.first().str())});
    auto J = llvm::orc::LLJITBuilder()
                 .setJITTargetMachineBuilder(std::move(JTMB))
                 .create();
    if (!J) {
      Err = llvm::toString(J.takeError());
      return false;
    }
    Jit = std::move(*J);
  }
  // No unwind tables: the JIT cannot register .eh_frame on this host.
  // The test calls the function with the Windows convention.
  for (llvm::Function &Fn : *M) {
    Fn.addFnAttr(llvm::Attribute::NoUnwind);
    Fn.setCallingConv(llvm::CallingConv::Win64);
  }
  M->setTargetTriple(JitTriple);
  M->setDataLayout(Jit->getDataLayout());
  auto JD = Jit->createJITDylib("case" + std::to_string(Serial++));
  if (!JD) {
    Err = llvm::toString(JD.takeError());
    return false;
  }
  if (std::getenv("REVENANT_SEM_IR"))
    M->print(llvm::errs(), nullptr);
  if (auto E = Jit->addIRModule(
          *JD, llvm::orc::ThreadSafeModule(std::move(M), std::move(Ctx)))) {
    Err = llvm::toString(std::move(E));
    return false;
  }
  auto Sym = Jit->lookup(*JD, Name);
  if (!Sym) {
    Err = llvm::toString(Sym.takeError());
    return false;
  }
  Out.assign(BufferQwords, 0);
  Sym->toPtr<void (*)(uint64_t *)>()(Out.data());
  return true;
}

TEST(LiftSemantics, MatchesNativeExecution) {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  // The host may lack the ISA a case exercises (this machine has no AVX-512),
  // so a case with `Requires` runs only when the host reports the feature;
  // otherwise the native half would fault.
  llvm::StringMap<bool> HostFeatures = llvm::sys::getHostCPUFeatures();
  std::vector<Case> Cases = cases();
  unsigned Failures = 0;
  unsigned Skipped = 0;
  const char *Only = std::getenv("REVENANT_SEM_FILTER");
  const bool Verbose = std::getenv("REVENANT_SEM_VERBOSE") != nullptr;
  for (const Case &C : Cases) {
    if (C.Requires && !HostFeatures.lookup(C.Requires)) {
      ++Skipped;
      if (Verbose)
        llvm::errs() << "skip " << C.Name << " (needs " << C.Requires << ")\n";
      continue;
    }
    if (Only && C.Name.find(Only) == std::string::npos)
      continue;
    if (Verbose)
      llvm::errs() << C.Name << "\n";
    for (bool VariantB : {false, true}) {
      std::string Err;
      std::vector<uint8_t> Code;
      if (!assemble(snippet(C, VariantB), Code, Err)) {
        ADD_FAILURE() << C.Name << ": assemble: " << Err;
        ++Failures;
        break;
      }
      if (Verbose)
        llvm::errs() << "  native\n";
      std::vector<uint64_t> Want = runNative(Code), Got;
      if (Verbose)
        llvm::errs() << "  lifted\n";
      if (!runLifted(Code, Got, Err)) {
        ADD_FAILURE() << C.Name << ": lift: " << Err;
        ++Failures;
        break;
      }
      std::string Diff;
      for (unsigned K = 0; K < 10; ++K)
        if ((C.RegMask >> K & 1) && Want[K] != Got[K])
          Diff += std::string(" ") + RegName[K] + " want " + hexQ(Want[K]) +
                  " got " + hexQ(Got[K]) + ";";
      const char *FlagNames[5] = {"CF", "ZF", "SF", "OF", "PF"};
      for (unsigned K = 0; K < 5; ++K)
        if ((C.FlagMask >> K & 1) && Want[SlotFlags + K] != Got[SlotFlags + K])
          Diff += std::string(" ") + FlagNames[K] + " want " +
                  std::to_string(Want[SlotFlags + K]) + " got " +
                  std::to_string(Got[SlotFlags + K]) + ";";
      for (unsigned K = 0; K < C.Stack.size(); ++K)
        if (Want[SlotStack + K] != Got[SlotStack + K])
          Diff += " stack" + std::to_string(K) + " want " +
                  hexQ(Want[SlotStack + K]) + " got " +
                  hexQ(Got[SlotStack + K]) + ";";
      for (unsigned N = 0; N < VecRegs; ++N) {
        if (!C.VecUsed)
          continue;
        unsigned NQ = C.Zmm ? 8u : C.Ymm ? 4u : 2u;
        for (unsigned Q = 0; Q < NQ; ++Q) {
          uint64_t W = Want[SlotVec + N * VecQwords + Q];
          uint64_t G = Got[SlotVec + N * VecQwords + Q];
          if (W != G)
            Diff += (C.Zmm ? " zmm" : C.Ymm ? " ymm" : " xmm") +
                    std::to_string(N) + "." + std::to_string(Q) + " want " +
                    hexQ(W) + " got " + hexQ(G) + ";";
        }
      }
      if (!Diff.empty()) {
        ADD_FAILURE() << C.Name << (VariantB ? " [flags B]" : " [flags A]")
                      << ":" << Diff;
        ++Failures;
        break;
      }
    }
  }
  llvm::errs() << Cases.size() << " cases, " << Failures << " failed, "
               << Skipped << " skipped\n";
  EXPECT_EQ(Failures, 0u);
}

} // end anonymous namespace
