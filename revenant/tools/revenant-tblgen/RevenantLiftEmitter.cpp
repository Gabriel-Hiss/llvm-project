#include "Common/CodeGenDAGPatterns.h"
#include "Common/CodeGenInstruction.h"
#include "Common/CodeGenTarget.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Compiler.h"
#include "llvm/TableGen/Record.h"
#include "llvm/TableGen/TableGenBackend.h"
#include <map>
#include <set>

using namespace llvm;

namespace {

class RevenantLiftEmitter {
  const RecordKeeper &Records;
  CodeGenDAGPatterns Patterns;

  static const DagInit *bodyOf(const Record &R) {
    return R.getValueAsDag("Body");
  }

  static const Record *opOf(const DagInit &D) {
    const auto *DI = dyn_cast<DefInit>(D.getOperator());
    return DI ? DI->getDef() : nullptr;
  }

  /// Width of the widest vector register class among the operands, 0 when
  /// the instruction has none.
  static unsigned vectorWidthOf(const CodeGenInstruction &I) {
    unsigned W = 0;
    for (unsigned K = 0, E = I.Operands.size(); K != E; ++K) {
      StringRef C = I.Operands[K].Rec->getName();
      unsigned Bits = C.starts_with("VR512")   ? 512
                      : C.starts_with("VR256") ? 256
                      : C.starts_with("VR128") ? 128
                                               : 0;
      W = std::max(W, Bits);
    }
    return W;
  }

  static unsigned widthFor(const Record &Insn) {
    StringRef Name = Insn.getName();
    if (Name.starts_with("MOVSS") || Name.starts_with("VMOVSS"))
      return 32;
    if ((Name.starts_with("MOVSD") || Name.starts_with("VMOVSD")) &&
        !Name.contains("to64"))
      return 64;
    if (Name.starts_with("MOVMSK") || Name.starts_with("PMOVMSKB"))
      return 32;
    if (Name.starts_with("V") && Name.contains('Z'))
      return 512;
    if (Name.starts_with("V") && Name.contains('Y'))
      return 256;
    if (Name.starts_with("V") || Name.contains("PS") ||
        Name.contains("PD") || Name.contains("DQ") ||
        Name.contains("XMM") || Name.starts_with("MOVAP") ||
        Name.starts_with("MOVUP") || Name.starts_with("MOVDQ"))
      return 128;
    for (unsigned I = 0; I < Name.size(); ++I) {
      if (!isDigit(Name[I]))
        continue;
      unsigned Width = 0;
      if (!Name.substr(I).take_while([](char C) { return isDigit(C); })
               .getAsInteger(10, Width))
        return Width == 8 || Width == 16 || Width == 32 || Width == 64 ||
                       Width == 80 || Width == 128 || Width == 256 ||
                       Width == 512
                   ? Width
                   : 64;
    }
    return 64;
  }

  /// Source width of a MOVSX/MOVZX/MOVSXD form: the digits that end the
  /// opcode name (MOVSX64rm8 -> 8, MOVSXD64rr32 -> 32); 0 for anything else.
  static unsigned extensionSourceBits(const CodeGenInstruction &I) {
    StringRef Name = I.TheDef->getName();
    if (!Name.starts_with("MOVSX") && !Name.starts_with("MOVZX"))
      return 0;
    size_t Begin = Name.size();
    while (Begin > 0 && isDigit(Name[Begin - 1]))
      --Begin;
    unsigned Bits = 0;
    if (Begin == Name.size() || Name.substr(Begin).getAsInteger(10, Bits))
      return 0;
    return Bits;
  }

  /// MI operand index of the N-th input (`rv_op N`). An address group is one
  /// input but five MI operands, so the index is not NumDefs + N.
  static unsigned inputOperandNo(const CodeGenInstruction &I, unsigned N) {
    unsigned K = I.Operands.NumDefs + N;
    return K < I.Operands.size() ? I.Operands[K].MIOperandNo : K;
  }

  static const CGIOperandList::OperandInfo *
  operandAt(const CodeGenInstruction &I, unsigned MIOpNo) {
    for (unsigned K = 0, E = I.Operands.size(); K != E; ++K)
      if (I.Operands[K].MIOperandNo == MIOpNo)
        return &I.Operands[K];
    return nullptr;
  }

  /// True for a five-operand x86 address group.
  static bool isMemOperand(const CodeGenInstruction &I, unsigned MIOpNo) {
    const auto *Op = operandAt(I, MIOpNo);
    return Op && Op->MINumOperands == 5;
  }

  static bool isRegisterOperand(const CodeGenInstruction &I, unsigned MIOpNo) {
    const auto *Op = operandAt(I, MIOpNo);
    return Op && Op->Rec->isSubClassOf("RegisterClass");
  }

  /// MI operand index of logical operand `L`: the operands in order with
  /// tied inputs merged into their defs, the iced-like view the DSL's
  /// `$dst` and `$src` roles refer to.
  static std::optional<unsigned> logicalOperandNo(const CodeGenInstruction &I,
                                                  unsigned L) {
    unsigned Seen = 0;
    for (unsigned K = 0, E = I.Operands.size(); K != E; ++K) {
      if (K >= I.Operands.NumDefs && I.Operands[K].getTiedRegister() != -1)
        continue;
      if (Seen++ == L)
        return I.Operands[K].MIOperandNo;
    }
    return std::nullopt;
  }

  /// MI operand index for a DSL operand name. TableGen names differ per
  /// form ($src1 vs $dst on RMW memory forms, $src vs $src1 on unary
  /// ops), so a name the instruction does not declare resolves by role:
  /// `$dst` is the first logical operand and `$src` the second.
  static std::optional<unsigned> operandNo(const CodeGenInstruction &I,
                                           StringRef Name) {
    if (Name.empty())
      return std::nullopt;
    if (auto N = I.Operands.findOperandNamed(Name))
      return I.Operands[*N].MIOperandNo;
    if (auto Sub = I.Operands.findSubOperandAlias(Name)) {
      const auto &[OpIdx, SubIdx] = *Sub;
      return I.Operands[OpIdx].MIOperandNo + SubIdx;
    }
    if (Name == "dst")
      return logicalOperandNo(I, 0);
    if (Name == "src")
      return logicalOperandNo(I, 1);
    return std::nullopt;
  }

  /// Declared bit width of the operand a leaf names ($name or (rv_op N)):
  /// the trailing digits of the operand record (GR64/i64mem -> 64). 0 when
  /// the argument is not a bare operand or the record has no width digits.
  static unsigned declaredBits(const CodeGenInstruction &I, const Init *Arg,
                               StringRef Name) {
    std::optional<unsigned> No;
    if (!Name.empty())
      No = operandNo(I, Name);
    else if (const auto *D = dyn_cast<DagInit>(Arg)) {
      const Record *Op = opOf(*D);
      const auto *N = Op && Op->getName() == "rv_op" && D->getNumArgs() == 1
                          ? dyn_cast<IntInit>(D->getArg(0))
                          : nullptr;
      if (N)
        No = inputOperandNo(I, N->getValue());
    }
    if (!No)
      return 0;
    const auto *Info = operandAt(I, *No);
    if (!Info)
      return 0;
    StringRef R = Info->Rec->getName();
    size_t End = R.size();
    while (End > 0 && isDigit(R[End - 1]))
      --End;
    unsigned B = 0;
    if (End == R.size() || R.substr(End).getAsInteger(10, B))
      return 0;
    return B;
  }

  static std::string operandValue(const CodeGenInstruction &I, StringRef Name,
                                  unsigned Width) {
    auto N = operandNo(I, Name);
    if (!N) {
      // Shift and double-shift counts for the rCL/mCL forms live in the
      // implicit CL use, not in an operand.
      if (Name == "src2" || Name == "src3" || Name == "cnt")
        for (const Record *Use : I.ImplicitUses)
          if (Use->getName() == "CL")
            return (Twine("LC.regRead(X86::CL, LC.iN(") + Twine(Width) +
                    "))")
                .str();
      return {};
    }
    return (Twine("LC.readOp(MI, ") + Twine(*N) +
            ", LC.iN(" + Twine(Width) + "))")
        .str();
  }


  static int conditionCode(StringRef CC) {
    if (CC == "o") return 0;
    if (CC == "no") return 1;
    if (CC == "b") return 2;
    if (CC == "ae" || CC == "nb") return 3;
    if (CC == "e" || CC == "z") return 4;
    if (CC == "ne" || CC == "nz") return 5;
    if (CC == "be") return 6;
    if (CC == "a" || CC == "nbe") return 7;
    if (CC == "s") return 8;
    if (CC == "ns") return 9;
    if (CC == "p") return 10;
    if (CC == "np") return 11;
    if (CC == "l") return 12;
    if (CC == "ge") return 13;
    if (CC == "le") return 14;
    if (CC == "g") return 15;
    return -1;
  }

  static StringRef flagEnum(StringRef Flag) {
    if (Flag == "cf") return "CF";
    if (Flag == "zf") return "ZF";
    if (Flag == "sf") return "SF";
    if (Flag == "of") return "OF";
    if (Flag == "pf") return "PF";
    if (Flag == "af") return "AF";
    if (Flag == "df") return "DF";
    return {};
  }

  static std::string x87IndexExpr(const Init *I,
                                  const CodeGenInstruction &Inst) {
    if (const auto *N = dyn_cast<IntInit>(I))
      return Twine(N->getValue()).str();
    const auto *D = dyn_cast<DagInit>(I);
    const Record *Op = D ? opOf(*D) : nullptr;
    if (!Op || Op->getName() != "rv_x87idx" || D->getNumArgs() != 1)
      return {};
    auto N = operandNo(Inst, D->getArgNameStr(0));
    return N ? (Twine("LC.x87IndexOf(MI.getOperand(") + Twine(*N) +
                   ").getReg())")
                  .str()
             : std::string();
  }

  static std::string immediateExpr(const Init *I,
                                   const CodeGenInstruction &Inst) {
    if (const auto *N = dyn_cast<IntInit>(I))
      return Twine(N->getValue()).str();
    const auto *D = dyn_cast<DagInit>(I);
    const Record *Op = D ? opOf(*D) : nullptr;
    if (!Op || Op->getName() != "rv_op" || D->getNumArgs() != 1)
      return {};
    const auto *N = dyn_cast<IntInit>(D->getArg(0));
    if (!N)
      return {};
    return (Twine("MI.getOperand(") +
            Twine(inputOperandNo(Inst, N->getValue())) + ").getImm()")
        .str();
  }

  static std::string expression(const Init *I, const CodeGenInstruction &Inst,
                                unsigned Width, StringRef Named = {}) {
    if (!Named.empty())
      return operandValue(Inst, Named, Width);
    if (const auto *V = dyn_cast<VarInit>(I))
      return operandValue(Inst, V->getName(), Width);
    if (const auto *N = dyn_cast<IntInit>(I))
      return (Twine("LC.constI(") + Twine(N->getValue()) + ", " +
              Twine(Width) + ")")
          .str();
    if (const auto *D = dyn_cast<DagInit>(I)) {
      const Record *Op = opOf(*D);
      if (!Op)
        return {};
      StringRef Name = Op->getName();
      if (Name == "rv_x87st" && D->getNumArgs() == 1) {
        std::string Index = x87IndexExpr(D->getArg(0), Inst);
        return Index.empty() ? std::string()
                             : (Twine("LC.x87St(") + Index + ")").str();
      }
      if (Name == "rv_x87idx" && D->getNumArgs() == 1)
        return x87IndexExpr(I, Inst);
      if (Name == "rv_x87pop")
        return "LC.x87Pop()";
      if (Name == "rv_x87fldconst" && D->getNumArgs() == 1) {
        const auto *S = dyn_cast<StringInit>(D->getArg(0));
        return S ? (Twine("LC.x87Const(\"") + S->getValue() + "\")").str()
                 : std::string();
      }
      if (Name == "rv_x87math" &&
          (D->getNumArgs() == 2 || D->getNumArgs() == 3)) {
        const auto *S = dyn_cast<StringInit>(D->getArg(0));
        if (!S)
          return {};
        std::string A =
            expression(D->getArg(1), Inst, 80, D->getArgNameStr(1));
        if (A.empty())
          return {};
        if (D->getNumArgs() == 2)
          return (Twine("LC.x87Math(\"") + S->getValue() + "\", " + A +
                  ")")
              .str();
        std::string B =
            expression(D->getArg(2), Inst, 80, D->getArgNameStr(2));
        return B.empty()
                   ? std::string()
                   : (Twine("LC.x87Math(\"") + S->getValue() + "\", " + A +
                      ", " + B + ")")
                         .str();
      }
      if (Name == "rv_fstsw")
        return "LC.x87StatusWord()";
      if (Name == "rv_fld" && D->getNumArgs() == 2) {
        const auto *Kind = dyn_cast<StringInit>(D->getArg(0));
        auto Mem = operandNo(Inst, D->getArgNameStr(1));
        if (!Kind || !Mem)
          return {};
        StringRef K = Kind->getValue();
        unsigned Bits = K.ends_with("80") ? 80
                        : K.ends_with("64") ? 64
                        : K.ends_with("32") ? 32
                        : K.ends_with("16") ? 16
                        : K.ends_with("i64") ? 64
                        : K.ends_with("i32") ? 32
                        : K.ends_with("i16") ? 16
                                             : 0;
        if (!Bits)
          return {};
        std::string Loaded =
            (Twine("LC.memRead(MI, ") + Twine(*Mem) + ", " +
             (K.starts_with("f") ? (Twine("LC.fltTy(") + Twine(Bits) + ")")
                                      .str()
                                 : (Twine("LC.iN(") + Twine(Bits) + ")").str()) +
             ")")
                .str();
        if (K.starts_with("f80"))
          return Loaded;
        if (K.starts_with("f32") || K.starts_with("f64"))
          return (Twine("LC.builder().CreateFPExt(") + Loaded +
                  ", LC.fltTy(80))")
              .str();
        return (Twine("LC.builder().CreateSIToFP(") + Loaded +
                ", LC.fltTy(80))")
            .str();
      }
      if (Name == "rv_xlane0f32" || Name == "rv_xlane0f64" ||
          Name == "rv_xnewlane0f32" || Name == "rv_xnewlane0f64" ||
          Name == "rv_pmovmskb" || Name == "rv_movmskps" ||
          Name == "rv_movmskpd" || Name == "rv_vbroadcastb" ||
          Name == "rv_vcvtdq2pd" || Name == "rv_fsqrt" ||
          Name == "rv_sitofpf32" || Name == "rv_sitofpf64" ||
          Name == "rv_fpext" || Name == "rv_fptrunc") {
        if (D->getNumArgs() != 1)
          return {};
        bool NewLane = Name == "rv_xnewlane0f32" ||
                       Name == "rv_xnewlane0f64";
        unsigned W = Name.ends_with("f32") ? 32
                     : Name.ends_with("f64") ? 64
                                              : Width;
        // sitofp must read the source at its declared width: GR64/i64mem
        // sources are 64-bit even under rv_sitofpf32 (VCVTSI642SS would
        // otherwise truncate the operand before converting).
        if (Name == "rv_sitofpf32" || Name == "rv_sitofpf64")
          if (unsigned B =
                  declaredBits(Inst, D->getArg(0), D->getArgNameStr(0)))
            W = B;
        std::string A = expression(D->getArg(0), Inst, W,
                                   D->getArgNameStr(0));
        if (A.empty())
          return {};
        StringRef Vop = Name.drop_front(3);
        return NewLane ? (Twine("LC.vop(\"") + Vop +
                          "\", nullptr, " + A + ")")
                             .str()
                       : (Twine("LC.vop(\"") + Vop + "\", " + A + ")")
                             .str();
      }
      if ((Name == "rv_xsetlane0f32" || Name == "rv_xsetlane0f64") &&
          D->getNumArgs() == 2) {
        unsigned W = Name.ends_with("f32") ? 32 : 64;
        std::string V = expression(D->getArg(0), Inst, 128,
                                   D->getArgNameStr(0));
        std::string F = expression(D->getArg(1), Inst, W,
                                   D->getArgNameStr(1));
        return V.empty() || F.empty()
                   ? std::string()
                   : (Twine("LC.vop(\"") + Name.drop_front(3) + "\", " + V +
                      ", " + F + ")")
                         .str();
      }
      if ((Name == "rv_floadf32" || Name == "rv_floadf64") &&
          D->getNumArgs() == 1) {
        unsigned W = Name == "rv_floadf32" ? 32 : 64;
        std::string Addr = expression(D->getArg(0), Inst, 64,
                                      D->getArgNameStr(0));
        return Addr.empty()
                   ? std::string()
                   : (Twine("LC.memReadAddr(") + Addr + ", LC.fltTy(" +
                      Twine(W) + "))")
                         .str();
      }
      if (Name == "rv_fptosi" && D->getNumArgs() == 2) {
        const auto *W = dyn_cast<IntInit>(D->getArg(0));
        if (!W)
          return {};
        std::string F = expression(D->getArg(1), Inst, 64,
                                   D->getArgNameStr(1));
        return F.empty()
                   ? std::string()
                   : (Twine("LC.builder().CreateFPToSI(") + F + ", LC.iN(" +
                      Twine(W->getValue()) + "))")
                         .str();
      }
      if ((Name == "rv_pshufd" || Name == "rv_pshufhw" ||
           Name == "rv_pshuflw" || Name == "rv_pslldq" ||
           Name == "rv_psrldq") &&
          D->getNumArgs() == 2) {
        std::string V =
            expression(D->getArg(0), Inst, 128, D->getArgNameStr(0));
        std::string Imm = immediateExpr(D->getArg(1), Inst);
        return V.empty() || Imm.empty()
                   ? std::string()
                   : (Twine("LC.vop(\"") + Name.drop_front(3) + "\", " + V +
                      ", nullptr, " + Imm + ")")
                         .str();
      }
      if ((Name == "rv_shufps" || Name == "rv_shufpd" ||
           Name == "rv_pshufb") &&
          D->getNumArgs() == (Name == "rv_pshufb" ? 2u : 3u)) {
        std::string A = expression(D->getArg(0), Inst, 128,
                                   D->getArgNameStr(0));
        std::string B = expression(D->getArg(1), Inst, 128,
                                   D->getArgNameStr(1));
        if (A.empty() || B.empty())
          return {};
        if (Name == "rv_pshufb")
          return (Twine("LC.vop(\"pshufb\", ") + A + ", " + B + ")").str();
        std::string Imm = immediateExpr(D->getArg(2), Inst);
        return Imm.empty()
                   ? std::string()
                   : (Twine("LC.vop(\"") + Name.drop_front(3) + "\", " + A +
                      ", " + B + ", " + Imm + ")")
                         .str();
      }
      if ((Name == "rv_punpckl" || Name == "rv_punpckh") &&
          D->getNumArgs() == 3) {
        const auto *Bits = dyn_cast<IntInit>(D->getArg(0));
        std::string A =
            expression(D->getArg(1), Inst, 128, D->getArgNameStr(1));
        std::string B =
            expression(D->getArg(2), Inst, 128, D->getArgNameStr(2));
        return !Bits || A.empty() || B.empty()
                   ? std::string()
                   : (Twine("LC.vop(\"") + Name.drop_front(3) + "\", " + A +
                      ", " + B + ", " + Twine(Bits->getValue()) + ")")
                         .str();
      }
      if (Name == "rv_pinsrw" && D->getNumArgs() == 3) {
        std::string A =
            expression(D->getArg(0), Inst, 128, D->getArgNameStr(0));
        std::string B =
            expression(D->getArg(1), Inst, 16, D->getArgNameStr(1));
        std::string Imm = immediateExpr(D->getArg(2), Inst);
        return A.empty() || B.empty() || Imm.empty()
                   ? std::string()
                   : (Twine("LC.vop(\"pinsrw\", ") + A + ", " + B + ", " +
                      Imm + ")")
                         .str();
      }
      if (Name == "rv_vshift" && D->getNumArgs() == 4) {
        const auto *Kind = dyn_cast<StringInit>(D->getArg(0));
        const auto *Bits = dyn_cast<IntInit>(D->getArg(1));
        std::string V =
            expression(D->getArg(2), Inst, 128, D->getArgNameStr(2));
        std::string C =
            expression(D->getArg(3), Inst, 64, D->getArgNameStr(3));
        return !Kind || !Bits || V.empty() || C.empty()
                   ? std::string()
                   : (Twine("LC.vop(\"") + Kind->getValue() + "\", " + V +
                      ", " + C + ", " + Twine(Bits->getValue()) + ")")
                         .str();
      }
      if (Name == "rv_hasrep" || Name == "rv_hasrepe" ||
          Name == "rv_hasrepne" || Name == "rv_haslock") {
        StringRef Method = Name == "rv_hasrep" ? "hasAnyRep"
                           : Name == "rv_hasrepe" ? "hasRepE"
                           : Name == "rv_hasrepne" ? "hasRepNE"
                                                  : "hasLock";
        return (Twine("LC.constI(LC.") + Method + "(), 1)").str();
      }
      if (Name == "rv_df")
        return "LC.flagRead(X86LiftContext::DF)";

      if (Name == "rv_atomicrmw" && D->getNumArgs() == 3) {
        const auto *Kind = dyn_cast<StringInit>(D->getArg(0));
        std::string Addr =
            expression(D->getArg(1), Inst, 64, D->getArgNameStr(1));
        std::string Value =
            expression(D->getArg(2), Inst, Width, D->getArgNameStr(2));
        return !Kind || Addr.empty() || Value.empty()
                   ? std::string()
                   : (Twine("LC.atomicRMW(\"") + Kind->getValue() + "\", " +
                      Addr + ", " + Value + ")")
                         .str();
      }
      if (Name == "rv_cmpxchg" && D->getNumArgs() == 3) {
        std::string Addr =
            expression(D->getArg(0), Inst, 64, D->getArgNameStr(0));
        std::string Expected =
            expression(D->getArg(1), Inst, Width, D->getArgNameStr(1));
        std::string New =
            expression(D->getArg(2), Inst, Width, D->getArgNameStr(2));
        return Addr.empty() || Expected.empty() || New.empty()
                   ? std::string()
                   : (Twine("LC.cmpxchgOp(") + Addr + ", " + Expected +
                      ", " + New + ")")
                         .str();
      }
      if (Name == "rv_rorx" && D->getNumArgs() == 2) {
        std::string V =
            expression(D->getArg(0), Inst, Width, D->getArgNameStr(0));
        std::string Count =
            expression(D->getArg(1), Inst, 8, D->getArgNameStr(1));
        return V.empty() || Count.empty()
                   ? std::string()
                   : (Twine("LC.rorxOp(") + V + ", " + Count + ")").str();
      }
      if (Name == "rv_bextr" && D->getNumArgs() == 2) {
        std::string V =
            expression(D->getArg(0), Inst, Width, D->getArgNameStr(0));
        std::string C =
            expression(D->getArg(1), Inst, Width, D->getArgNameStr(1));
        return V.empty() || C.empty()
                   ? std::string()
                   : (Twine("LC.bextrOp(") + V + ", " + C + ")").str();
      }
      if (Name == "rv_bzhi" && D->getNumArgs() == 2) {
        std::string V =
            expression(D->getArg(0), Inst, Width, D->getArgNameStr(0));
        std::string C =
            expression(D->getArg(1), Inst, Width, D->getArgNameStr(1));
        return V.empty() || C.empty()
                   ? std::string()
                   : (Twine("LC.bzhiOp(") + V + ", " + C + ")").str();
      }
      if (Name == "rv_bls" && D->getNumArgs() == 2) {
        const auto *Kind = dyn_cast<StringInit>(D->getArg(0));
        std::string V =
            expression(D->getArg(1), Inst, Width, D->getArgNameStr(1));
        return !Kind || V.empty()
                   ? std::string()
                   : (Twine("LC.blsOp(\"") + Kind->getValue() + "\", " + V +
                      ")")
                         .str();
      }
      if (Name == "rv_shiftx" && D->getNumArgs() == 3) {
        const auto *Kind = dyn_cast<StringInit>(D->getArg(0));
        std::string V =
            expression(D->getArg(1), Inst, Width, D->getArgNameStr(1));
        std::string C =
            expression(D->getArg(2), Inst, Width, D->getArgNameStr(2));
        return !Kind || V.empty() || C.empty()
                   ? std::string()
                   : (Twine("LC.shiftXOp(\"") + Kind->getValue() + "\", " +
                      V + ", " + C + ")")
                         .str();
      }
      if ((Name == "rv_adcx" || Name == "rv_adox") && D->getNumArgs() == 2) {
        std::string A =
            expression(D->getArg(0), Inst, Width, D->getArgNameStr(0));
        std::string B =
            expression(D->getArg(1), Inst, Width, D->getArgNameStr(1));
        return A.empty() || B.empty()
                   ? std::string()
                   : (Twine("LC.carryAddOp(") + A + ", " + B +
                      (Name == "rv_adox" ? ", true)" : ", false)"))
                         .str();
      }
      if (Name == "rv_bt" && D->getNumArgs() == 3) {
        const auto *Kind = dyn_cast<StringInit>(D->getArg(0));
        auto BaseOp = operandNo(Inst, D->getArgNameStr(1));
        auto OffOp = operandNo(Inst, D->getArgNameStr(2));
        std::string Base =
            expression(D->getArg(1), Inst, Width, D->getArgNameStr(1));
        std::string Off =
            expression(D->getArg(2), Inst, 64, D->getArgNameStr(2));
        if (!Kind || !BaseOp || !OffOp || Base.empty() || Off.empty())
          return {};
        // A register offset into a memory base addresses a bit string
        // across elements and the helper stores the modified element
        // itself. An immediate offset wraps inside the loaded operand and
        // the caller writes the result back.
        bool BitString = isMemOperand(Inst, *BaseOp) &&
                         !isMemOperand(Inst, *OffOp) &&
                         isRegisterOperand(Inst, *OffOp);
        if (BitString)
          Off = (Twine("LC.builder().CreateSExt(LC.readOp(MI, ") +
                 Twine(*OffOp) + ", LC.iN(" + Twine(Width) + ")), LC.iN(64))")
                    .str();
        return (Twine("LC.bitTestOp(\"") + Kind->getValue() + "\", " + Base +
                ", " + Off + ", " + (BitString ? "true" : "false") + ", " +
                (BitString ? (Twine("LC.memAddr(MI, ") + Twine(*BaseOp) + ")")
                                 .str()
                           : std::string("nullptr")) +
                ", " + Twine(Width) + ")")
            .str();
      }
      if (Name == "rv_flagread" && D->getNumArgs() == 1) {
        const auto *Flag = dyn_cast<StringInit>(D->getArg(0));
        if (!Flag || flagEnum(Flag->getValue()).empty())
          return {};
        return (Twine("LC.flagRead(X86LiftContext::") +
                flagEnum(Flag->getValue()) + ")")
            .str();
      }
      if (Name == "rv_cpred" && D->getNumArgs() == 1) {
        if (const auto *CC = dyn_cast<StringInit>(D->getArg(0))) {
          int Code = conditionCode(CC->getValue());
          return Code < 0
                     ? std::string()
                     : (Twine("LC.condPred(") + Twine(Code) + ")").str();
        }
        auto N = operandNo(Inst, D->getArgNameStr(0));
        return N ? (Twine("LC.condPredOp(MI, ") + Twine(*N) + ")").str()
                 : std::string();
      }
      if (Name == "rv_setcc" && D->getNumArgs() == 1) {
        if (const auto *CC = dyn_cast<StringInit>(D->getArg(0))) {
          int Code = conditionCode(CC->getValue());
          return Code < 0
                     ? std::string()
                     : (Twine("LC.setccVal(") + Twine(Code) + ")").str();
        }
        auto N = operandNo(Inst, D->getArgNameStr(0));
        return N ? (Twine("LC.builder().CreateZExt(LC.condPredOp(MI, ") +
                    Twine(*N) + "), LC.iN(8))")
                       .str()
                 : std::string();
      }
      if (Name == "rv_flags")
        return "LC.eflagsIn()";
      if (Name == "rv_pop" && D->getNumArgs() == 1) {
        const auto *W = dyn_cast<IntInit>(D->getArg(0));
        return W ? (Twine("LC.popVal(") + Twine(W->getValue()) + ")").str()
                 : std::string();
      }
      if (Name == "rv_lahf")
        return "LC.lahfVal()";

      if (Name == "rv_reg" && D->getNumArgs() == 1) {
        const auto *S = dyn_cast<StringInit>(D->getArg(0));
        if (!S)
          return {};
        return (Twine("LC.regRead(X86::") + S->getValue() +
                ", LC.iN(" + Twine(Width) + "))")
            .str();
      }
      if ((Name == "rv_op" || Name == "rv_out") &&
          D->getNumArgs() == 1) {
        const auto *N = dyn_cast<IntInit>(D->getArg(0));
        if (!N)
          return {};
        unsigned Index = N->getValue();
        if (Name == "rv_op")
          Index = inputOperandNo(Inst, Index);
        return (Twine("LC.readOp(MI, ") + Twine(Index) +
                ", LC.iN(" + Twine(Width) + "))")
            .str();
      }
      if ((Name == "rv_imm" || Name == "rv_immw") &&
          D->getNumArgs() >= 1) {
        unsigned Arg = D->getNumArgs() - 1;
        const auto *N = dyn_cast<IntInit>(D->getArg(Arg));
        unsigned W = Width;
        if (Name == "rv_immw") {
          const auto *Bits = dyn_cast<IntInit>(D->getArg(0));
          if (!Bits)
            return {};
          W = Bits->getValue();
        }
        if (!N)
          return {};
        return (Twine("LC.constI(") + Twine(N->getValue()) + ", " +
                Twine(W) + ")")
            .str();
      }
      if (Name == "rv_addr" && D->getNumArgs() == 1) {
        StringRef OpName = D->getArgNameStr(0);
        auto N = operandNo(Inst, OpName);
        if (!N)
          return {};
        return (Twine("LC.memAddr(MI, ") + Twine(*N) + ")").str();
      }

      if (Name == "rv_load" && D->getNumArgs() == 2) {
        const auto *Bits = dyn_cast<IntInit>(D->getArg(0));
        if (!Bits)
          return {};
        std::string Addr =
            expression(D->getArg(1), Inst, 64, D->getArgNameStr(1));
        if (Addr.empty())
          return {};
        unsigned W = Bits->getValue() ? Bits->getValue() : Width;
        return (Twine("LC.memReadAddr(") + Addr + ", LC.iN(" + Twine(W) +
                "))")
            .str();
      }

      if (Name == "rv_vec" && D->getNumArgs() >= 3) {
        // (rv_vec "kind", ebits, args...): a typed vecOp over Width-bit
        // operands viewed as lanes of ebits.
        const auto *Kind = dyn_cast<StringInit>(D->getArg(0));
        const auto *EB = dyn_cast<IntInit>(D->getArg(1));
        if (!Kind || !EB || !EB->getValue() || Width % EB->getValue())
          return {};
        std::string Args;
        for (unsigned I = 2; I < D->getNumArgs(); ++I) {
          std::string A =
              expression(D->getArg(I), Inst, Width, D->getArgNameStr(I));
          if (A.empty())
            return {};
          Args += (I > 2 ? ", " : "") + A;
        }
        return (Twine("LC.vecOp(\"") + Kind->getValue() +
                "\", LC.vecTy(LC.iN(" + Twine(EB->getValue()) + "), " +
                Twine(Width / EB->getValue()) + "), {" + Args + "})")
            .str();
      }
      if (Name == "rv_fshift" && D->getNumArgs() == 3) {
        const auto *Kind = dyn_cast<StringInit>(D->getArg(0));
        if (!Kind)
          return {};
        std::string V =
            expression(D->getArg(1), Inst, Width, D->getArgNameStr(1));
        std::string Count =
            expression(D->getArg(2), Inst, 8, D->getArgNameStr(2));
        if (V.empty() || Count.empty())
          return {};
        return (Twine("LC.flagsShift(\"") + Kind->getValue() + "\", " + V +
                ", " + Count + ")")
            .str();
      }
      if (Name == "rv_fshift2" && D->getNumArgs() == 4) {
        const auto *Kind = dyn_cast<StringInit>(D->getArg(0));
        if (!Kind)
          return {};
        std::string V =
            expression(D->getArg(1), Inst, Width, D->getArgNameStr(1));
        std::string Extra =
            expression(D->getArg(2), Inst, Width, D->getArgNameStr(2));
        std::string Count =
            expression(D->getArg(3), Inst, 8, D->getArgNameStr(3));
        if (V.empty() || Extra.empty() || Count.empty())
          return {};
        return (Twine("LC.flagsShift2(\"") + Kind->getValue() + "\", " + V +
                ", " + Extra + ", " + Count + ")")
            .str();
      }
      if (Name == "rv_select" && D->getNumArgs() == 3) {
        std::string Cond =
            expression(D->getArg(0), Inst, 1, D->getArgNameStr(0));
        std::string A =
            expression(D->getArg(1), Inst, Width, D->getArgNameStr(1));
        std::string B =
            expression(D->getArg(2), Inst, Width, D->getArgNameStr(2));
        if (Cond.empty() || A.empty() || B.empty())
          return {};
        return (Twine("LC.builder().CreateSelect(") + Cond + ", " + A + ", " +
                B + ")")
            .str();
      }
      if (Name == "rv_zext" || Name == "rv_sext" || Name == "rv_trunc") {
        unsigned W = Width;
        if (D->getNumArgs() == 2) {
          const auto *Bits = dyn_cast<IntInit>(D->getArg(0));
          if (!Bits)
            return {};
          W = Bits->getValue();
        } else if (D->getNumArgs() != 1) {
          return {};
        }
        // MOVSX/MOVZX read the narrow source (the trailing digits of the
        // opcode name), not a destination-wide value.
        unsigned ReadW = Width;
        if (D->getNumArgs() == 1 && Name != "rv_trunc")
          if (unsigned Src = extensionSourceBits(Inst))
            ReadW = Src;
        std::string V =
            expression(D->getArg(D->getNumArgs() - 1), Inst, ReadW,
                       D->getArgNameStr(D->getNumArgs() - 1));
        if (V.empty())
          return {};
        StringRef Cast = Name == "rv_zext" ? "CreateZExtOrTrunc"
                         : Name == "rv_sext" ? "CreateSExtOrTrunc"
                                             : "CreateTrunc";
        return (Twine("LC.builder().") + Cast + "(" + V + ", LC.iN(" +
                Twine(W) + "))")
            .str();
      }

      if (D->getNumArgs() == 1) {
        std::string V =
            expression(D->getArg(0), Inst, Width, D->getArgNameStr(0));
        if (V.empty())
          return {};
        if (Name == "rv_not")
          return (Twine("LC.builder().CreateNot(") + V + ")").str();
        if (Name == "rv_finc")
          return (Twine("LC.flagsInc(") + V + ")").str();
        if (Name == "rv_fdec")
          return (Twine("LC.flagsDec(") + V + ")").str();
        if (Name == "rv_fnegx")
          return (Twine("LC.flagsNeg(") + V + ")").str();
        if (Name == "rv_bsf")
          return (Twine("LC.bsfOp(") + V + ")").str();
        if (Name == "rv_bsr")
          return (Twine("LC.bsrOp(") + V + ")").str();
        if (Name == "rv_tzcnt")
          return (Twine("LC.tzcntOp(") + V + ")").str();
        if (Name == "rv_lzcnt")
          return (Twine("LC.lzcntOp(") + V + ")").str();
        if (Name == "rv_popcnt")
          return (Twine("LC.popcntOp(") + V + ")").str();
        if (Name == "rv_bswap")
          return (Twine("LC.bswapOp(") + V + ")").str();
        if (Name == "rv_lahf")
          return "LC.lahfVal()";
      }

      if (Name == "rv_fma" && D->getNumArgs() == 4) {
        const auto *Mode = dyn_cast<StringInit>(D->getArg(0));
        if (!Mode)
          return {};
        std::string A =
            expression(D->getArg(1), Inst, Width, D->getArgNameStr(1));
        std::string B =
            expression(D->getArg(2), Inst, Width, D->getArgNameStr(2));
        std::string C =
            expression(D->getArg(3), Inst, Width, D->getArgNameStr(3));
        if (A.empty() || B.empty() || C.empty())
          return {};
        StringRef OpMode = Mode->getValue();
        bool NegProduct = OpMode.starts_with("n");
        bool NegAddend = OpMode.contains("sub");
        StringRef Opcode = Inst.TheDef->getName();
        bool Scalar = Opcode.contains("SS") || Opcode.contains("SD");
        unsigned EltBits = Opcode.contains("SD") || Opcode.contains("PD")
                               ? 64
                               : 32;
        if (OpMode.ends_with("132") || OpMode.ends_with("213"))
          std::swap(B, C);
        else if (OpMode.ends_with("231")) {
          std::string First = B;
          B = C;
          C = A;
          A = std::move(First);
        }
        return (Twine("LC.fmaOp(") + A + ", " + B + ", " + C +
                ", LC.readOp(MI, 0, LC.iN(" + Twine(Width) + ")), " +
                (NegProduct ? "true" : "false") + ", " +
                (NegAddend ? "true" : "false") + ", " +
                Twine(EltBits) + ", " + (Scalar ? "true" : "false") + ")")
            .str();
      }
      if (Name == "rv_icmp" && D->getNumArgs() == 3) {
        const auto *Pred = dyn_cast<StringInit>(D->getArg(0));
        if (!Pred)
          return {};
        std::string A =
            expression(D->getArg(1), Inst, Width, D->getArgNameStr(1));
        std::string B =
            expression(D->getArg(2), Inst, Width, D->getArgNameStr(2));
        if (A.empty() || B.empty())
          return {};
        StringRef Cmp;
        if (Pred->getValue() == "eq") Cmp = "CreateICmpEQ";
        else if (Pred->getValue() == "ne") Cmp = "CreateICmpNE";
        else if (Pred->getValue() == "ult") Cmp = "CreateICmpULT";
        else if (Pred->getValue() == "ule") Cmp = "CreateICmpULE";
        else if (Pred->getValue() == "ugt") Cmp = "CreateICmpUGT";
        else if (Pred->getValue() == "uge") Cmp = "CreateICmpUGE";
        else if (Pred->getValue() == "slt") Cmp = "CreateICmpSLT";
        else if (Pred->getValue() == "sle") Cmp = "CreateICmpSLE";
        else if (Pred->getValue() == "sgt") Cmp = "CreateICmpSGT";
        else if (Pred->getValue() == "sge") Cmp = "CreateICmpSGE";
        else return {};
        return (Twine("LC.builder().") + Cmp + "(" + A + ", " + B + ")")
            .str();
      }

      if (D->getNumArgs() == 2) {
        std::string A =
            expression(D->getArg(0), Inst, Width, D->getArgNameStr(0));
        std::string B =
            expression(D->getArg(1), Inst, Width, D->getArgNameStr(1));
        if (A.empty() || B.empty())
          return {};
        StringRef Builder;
        if (Name == "rv_add")
          Builder = "CreateAdd";
        else if (Name == "rv_sub")
          Builder = "CreateSub";
        else if (Name == "rv_mul")
          Builder = "CreateMul";
        else if (Name == "rv_and")
          Builder = "CreateAnd";
        else if (Name == "rv_or")
          Builder = "CreateOr";
        else if (Name == "rv_xor")
          Builder = "CreateXor";
        else if (Name == "rv_shl")
          Builder = "CreateShl";
        else if (Name == "rv_lshr")
          Builder = "CreateLShr";
        else if (Name == "rv_ashr")
          Builder = "CreateAShr";
        else if (Name == "rv_udiv")
          Builder = "CreateUDiv";
        else if (Name == "rv_sdiv")
          Builder = "CreateSDiv";
        else if (Name == "rv_urem")
          Builder = "CreateURem";
        else if (Name == "rv_srem")
          Builder = "CreateSRem";
        else if (Name == "rv_ffadd")
          Builder = "CreateFAdd";
        else if (Name == "rv_ffsub")
          Builder = "CreateFSub";
        else if (Name == "rv_ffmul")
          Builder = "CreateFMul";
        else if (Name == "rv_ffdiv")
          Builder = "CreateFDiv";
        else if (Name == "rv_fminnum" || Name == "rv_fmaxnum")
          return (Twine("LC.vop(\"") +
                  (Name == "rv_fminnum" ? "fminnum" : "fmaxnum") + "\", " +
                  A + ", " + B + ")")
              .str();

        else if (Name == "rv_fadd")
          return (Twine("LC.flagsAdd(") + A + ", " + B + ")").str();
        else if (Name == "rv_fsub")
          return (Twine("LC.flagsSub(") + A + ", " + B + ")").str();
        else if (Name == "rv_fadc")
          return (Twine("LC.flagsAdc(") + A + ", " + B + ")").str();
        else if (Name == "rv_fsbb")
          return (Twine("LC.flagsSbb(") + A + ", " + B + ")").str();
        else if (Name == "rv_fmul")
          return (Twine("LC.flagsSMul(") + A + ", " + B + ")").str();
        else if (Name == "rv_fumul")
          return (Twine("LC.flagsUMul(") + A + ", " + B + ")").str();
        else if (Name == "rv_fcmp")
          return (Twine("LC.flagsCmp(") + A + ", " + B + ")").str();

        else if (Name == "rv_fand" || Name == "rv_for" ||
                 Name == "rv_fxor") {
          StringRef Bin = Name == "rv_fand" ? "CreateAnd"
                          : Name == "rv_for" ? "CreateOr"
                                             : "CreateXor";
          return (Twine("LC.flagsLogic(LC.builder().") + Bin + "(" + A +
                  ", " + B + "))")
              .str();
        } else {
          return {};
        }
        return (Twine("LC.builder().") + Builder + "(" + A + ", " + B +
                ")")
            .str();
      }
    }
    return {};
  }


  static StringRef firstNamedOperand(const TreePatternNode &N) {
    if (N.hasName())
      return N.getName();
    if (N.isLeaf())
      return {};
    for (const TreePatternNode &Child : N.children()) {
      StringRef Name = firstNamedOperand(Child);
      if (!Name.empty())
        return Name;
    }
    return {};
  }

  static std::string treeExpression(const TreePatternNode &N,
                                    const CodeGenInstruction &Inst,
                                    unsigned Width) {
    if (N.isLeaf()) {
      if (N.hasName())
        return operandValue(Inst, N.getName(), Width);
      if (const auto *I = dyn_cast<IntInit>(N.getLeafValue()))
        return (Twine("LC.constI(") + Twine(I->getValue()) + ", " +
                Twine(Width) + ")")
            .str();
      return {};
    }
    StringRef Op = N.getOperator()->getName();
    if (Op == "ld" || Op == "load") {
      StringRef Name = firstNamedOperand(N);
      auto OpNo = operandNo(Inst, Name);
      return OpNo ? (Twine("LC.readOp(MI, ") + Twine(*OpNo) +
                      ", LC.iN(" + Twine(Width) + "))")
                         .str()
                   : std::string();
    }

    if (Op == "set" && N.getNumChildren() == 2)
      return treeExpression(N.getChild(1), Inst, Width);
    if (N.getNumChildren() != 2)
      return {};
    std::string A = treeExpression(N.getChild(0), Inst, Width);
    std::string B = treeExpression(N.getChild(1), Inst, Width);
    if (A.empty() || B.empty())
      return {};
    StringRef Builder;
    if (Op == "add") Builder = "CreateAdd";
    else if (Op == "sub") Builder = "CreateSub";
    else if (Op == "mul") Builder = "CreateMul";
    else if (Op == "and") Builder = "CreateAnd";
    else if (Op == "or") Builder = "CreateOr";
    else if (Op == "xor") Builder = "CreateXor";
    else if (Op == "shl") Builder = "CreateShl";
    else if (Op == "srl") Builder = "CreateLShr";
    else if (Op == "sra") Builder = "CreateAShr";
    else return {};
    return (Twine("LC.builder().") + Builder + "(" + A + ", " + B + ")")
        .str();
  }

  static bool hasNestedInst(const TreePatternNode &N) {
    if (N.isLeaf())
      return false;
    for (const TreePatternNode &Child : N.children()) {
      if (!Child.isLeaf() &&
          Child.getOperator()->isSubClassOf("Instruction"))
        return true;
      if (hasNestedInst(Child))
        return true;
    }
    return false;
  }


  //===------------------------------------------------------------------===//
  // Typed vector patterns
  //
  // A pattern whose value has a vector type is inverted with its machine
  // types kept: every operand is read as the node's vector type, ISD nodes
  // become IRBuilder calls and the X86ISD nodes with no single IR spelling go
  // through LC.vecOp. Anything not understood fails the whole pattern, so the
  // instruction stays opaque instead of lifting wrongly.
  //===------------------------------------------------------------------===//

  struct VecPat {
    const CodeGenInstruction &Inst;
    const TreePatternNode &Dst;
    bool Failed = false;
  };

  static bool isRegisterOp(const CGIOperandList::OperandInfo *Op) {
    return Op && Op->MINumOperands == 1 &&
           (Op->Rec->isSubClassOf("RegisterClass") ||
            Op->Rec->isSubClassOf("RegisterOperand"));
  }

  /// C++ expression for the llvm::Type of a machine type; empty if the type
  /// has no lifted representation (i1 lanes, unknown sizes).
  static std::string typeExpr(MVT VT) {
    if (VT.isVector()) {
      MVT E = VT.getVectorElementType();
      if (E == MVT::i1 || VT.isScalableVector())
        return {};
      std::string Elt = typeExpr(E);
      return Elt.empty() ? std::string()
                         : (Twine("LC.vecTy(") + Elt + ", " +
                            Twine(VT.getVectorNumElements()) + ")")
                               .str();
    }
    unsigned Bits = VT.getSizeInBits();
    if (VT.isFloatingPoint())
      return (Twine("LC.fltTy(") + Twine(Bits) + ")").str();
    if (VT.isInteger() && VT != MVT::i1)
      return (Twine("LC.iN(") + Twine(Bits) + ")").str();
    return {};
  }

  /// MI operand number a pattern name refers to: the instruction's own
  /// operand name, or the operand in the same position of the result DAG.
  static std::optional<unsigned> patOperandNo(VecPat &C, StringRef Name) {
    if (auto N = operandNo(C.Inst, Name))
      return N;
    for (unsigned K = 0, E = C.Dst.getNumChildren(); K != E; ++K) {
      const TreePatternNode &Ch = C.Dst.getChild(K);
      if (Ch.hasName() && Ch.getName() == Name)
        return inputOperandNo(C.Inst, K);
    }
    return std::nullopt;
  }

  static bool hasType(const TreePatternNode &N) {
    return N.getNumTypes() == 1 && N.hasConcreteType(0);
  }

  /// The immediate a node supplies as an instruction operand: a literal, a
  /// named immediate operand, or an unnamed constant whose value the result
  /// DAG computes through an SDNodeXForm (scaled by `Scale`).
  static std::string immArg(VecPat &C, const TreePatternNode &N,
                            unsigned Scale = 1) {
    if (N.isLeaf() || N.getNumChildren() == 0) {
      if (N.hasName()) {
        auto No = patOperandNo(C, N.getName());
        if (No)
          return (Twine("MI.getOperand(") + Twine(*No) + ").getImm()").str();
      }
      if (N.isLeaf())
        if (const auto *I = dyn_cast<IntInit>(N.getLeafValue()))
          return Twine(I->getValue()).str();
    }
    for (unsigned K = 0, E = C.Dst.getNumChildren(); K != E; ++K) {
      const TreePatternNode &Ch = C.Dst.getChild(K);
      if (!Ch.isLeaf() && Ch.getOperator()->isSubClassOf("SDNodeXForm"))
        return (Twine("(MI.getOperand(") + Twine(inputOperandNo(C.Inst, K)) +
                ").getImm() * " + Twine(Scale) + ")")
            .str();
    }
    C.Failed = true;
    return {};
  }

  struct VecKind {
    const char *Node;
    const char *Kind;
    unsigned Vals; ///< value operands
    bool Imm;      ///< trailing immediate
  };

  static const VecKind *vecKind(StringRef Op) {
    static const VecKind Table[] = {
        {"X86pcmpeq", "cmpeq", 2, false},   {"X86pcmpgt", "cmpgt", 2, false},
        {"X86Blendv", "blendv", 3, false},  {"X86movmsk", "movmsk", 1, false},
        {"X86Unpckl", "unpckl", 2, false},  {"X86Unpckh", "unpckh", 2, false},
        {"X86PShufd", "pshufd", 1, true},   {"X86VPermilpi", "pshufd", 1, true},
        {"X86PShuflw", "pshuflw", 1, true}, {"X86PShufhw", "pshufhw", 1, true},
        {"X86Shufp", "shufp", 2, true},     {"X86PAlignr", "alignr", 2, true},
        {"X86VPerm2x128", "perm2x128", 2, true},
        {"X86VPermi", "permq", 1, true},    {"X86VPermv", "permv", 2, false},
        {"X86pshufb", "pshufb", 2, false},  {"X86Movss", "movs", 2, false},
        {"X86Movsd", "movs", 2, false},     {"X86Movlhps", "movlhps", 2, false},
        {"X86Movhlps", "movhlps", 2, false},
        {"X86vzmovl", "vzmovl", 1, false},  {"scalar_to_vector", "s2v", 1, false},
        {"X86VBroadcast", "broadcast", 1, false},
        {"X86fmin", "fmin", 2, false},      {"X86fmax", "fmax", 2, false},
        {"X86fand", "bitand", 2, false},    {"X86for", "bitor", 2, false},
        {"X86fxor", "bitxor", 2, false},    {"X86andnp", "andn", 2, false},
        {"X86fandn", "andn", 2, false},
        {"X86vshli", "shli", 1, true},      {"X86vsrli", "srli", 1, true},
        {"X86vsrai", "srai", 1, true},      {"X86vshl", "shl", 2, false},
        {"X86vsrl", "srl", 2, false},       {"X86vsra", "sra", 2, false},
        {"X86vshlv", "shlv", 2, false},     {"X86vsrlv", "srlv", 2, false},
        {"X86vsrav", "srav", 2, false},     {"zext_invec", "zext_invec", 1, false},
        {"sext_invec", "sext_invec", 1, false},
        {"X86cmpp", "cmpp", 2, true},       {"X86pmuludq", "muludq", 2, false},
        {"X86pmuldq", "muldq", 2, false},   {"avgceilu", "avg", 2, false},
        {"X86Packss", "packss", 2, false},  {"X86Packus", "packus", 2, false},
    };
    for (const VecKind &K : Table)
      if (Op == K.Node)
        return &K;
    return nullptr;
  }

  /// True when the node or anything below it has a vector type.
  static bool anyVectorType(const TreePatternNode &N) {
    if (hasType(N) && N.getSimpleType(0).isVector())
      return true;
    if (!N.isLeaf())
      for (const TreePatternNode &Ch : N.children())
        if (anyVectorType(Ch))
          return true;
    return false;
  }

  static std::string vecExpr(VecPat &C, const TreePatternNode &N) {
    if (C.Failed || !hasType(N)) {
      C.Failed = true;
      return {};
    }
    MVT VT = N.getSimpleType(0);
    std::string Ty = typeExpr(VT);
    if (Ty.empty()) {
      C.Failed = true;
      return {};
    }
    unsigned Bits = VT.getSizeInBits();
    auto Read = [&](unsigned No) {
      return (Twine("LC.vcast(LC.readOp(MI, ") + Twine(No) + ", LC.iN(" +
              Twine(Bits) + ")), " + Ty + ")")
          .str();
    };
    // Leaves and constants.
    if (N.isLeaf() || (N.getNumChildren() == 0 && N.hasName())) {
      if (N.hasName()) {
        auto No = patOperandNo(C, N.getName());
        if (!No) {
          C.Failed = true;
          return {};
        }
        const auto *Op = operandAt(C.Inst, *No);
        if (isMemOperand(C.Inst, *No))
          return (Twine("LC.memRead(MI, ") + Twine(*No) + ", " + Ty + ")")
              .str();
        if (isRegisterOp(Op) || (Op && !VT.isVector()))
          return Read(*No);
      }
      if (N.isLeaf())
        if (const auto *I = dyn_cast<IntInit>(N.getLeafValue()))
          if (!VT.isVector())
            return (Twine("LC.vcast(LC.constI(") + Twine(I->getValue()) + ", " +
                    Twine(Bits) + "), " + Ty + ")")
                .str();
      C.Failed = true;
      return {};
    }
    StringRef Op = N.getOperator()->getName();
    unsigned NC = N.getNumChildren();
    auto Child = [&](unsigned I) { return vecExpr(C, N.getChild(I)); };

    if (Op == "undef")
      return (Twine("UndefValue::get(") + Ty + ")").str();
    if ((Op == "ld" || Op == "load") && NC == 1) {
      auto No = patOperandNo(C, firstNamedOperand(N));
      if (!No || !isMemOperand(C.Inst, *No)) {
        C.Failed = true;
        return {};
      }
      return (Twine("LC.memRead(MI, ") + Twine(*No) + ", " + Ty + ")").str();
    }
    if (Op == "bitconvert" && NC == 1) {
      std::string A = Child(0);
      return A.empty() ? A : (Twine("LC.vcast(") + A + ", " + Ty + ")").str();
    }
    // Plain IRBuilder spellings.
    static const std::pair<StringRef, StringRef> Bin[] = {
        {"add", "CreateAdd"},   {"sub", "CreateSub"},  {"mul", "CreateMul"},
        {"and", "CreateAnd"},   {"or", "CreateOr"},    {"xor", "CreateXor"},
        {"shl", "CreateShl"},   {"srl", "CreateLShr"}, {"sra", "CreateAShr"},
        {"fadd", "CreateFAdd"}, {"fsub", "CreateFSub"},
        {"fmul", "CreateFMul"}, {"fdiv", "CreateFDiv"}};
    if (NC == 2)
      for (const auto &[Node, Builder] : Bin)
        if (Op == Node) {
          std::string A = Child(0), B = Child(1);
          if (A.empty() || B.empty())
            return {};
          return (Twine("LC.builder().") + Builder + "(LC.vcast(" + A + ", " +
                  Ty + "), LC.vcast(" + B + ", " + Ty + "))")
              .str();
        }
    static const std::pair<StringRef, StringRef> Intr[] = {
        {"smax", "smax"},       {"smin", "smin"},
        {"umax", "umax"},       {"umin", "umin"},
        {"uaddsat", "uadd_sat"}, {"saddsat", "sadd_sat"},
        {"usubsat", "usub_sat"}, {"ssubsat", "ssub_sat"}};
    if (NC == 2)
      for (const auto &[Node, Id] : Intr)
        if (Op == Node) {
          std::string A = Child(0), B = Child(1);
          if (A.empty() || B.empty())
            return {};
          return (Twine("LC.builder().CreateBinaryIntrinsic(Intrinsic::") + Id +
                  ", LC.vcast(" + A + ", " + Ty + "), LC.vcast(" + B + ", " +
                  Ty + "))")
              .str();
        }
    if (NC == 1 && (Op == "fsqrt" || Op == "fabs" || Op == "fneg")) {
      std::string A = Child(0);
      if (A.empty())
        return {};
      if (Op == "fneg")
        return (Twine("LC.builder().CreateFNeg(LC.vcast(") + A + ", " + Ty +
                "))")
            .str();
      return (Twine("LC.builder().CreateUnaryIntrinsic(Intrinsic::") +
              (Op == "fsqrt" ? "sqrt" : "fabs") + ", LC.vcast(" + A + ", " +
              Ty + "))")
          .str();
    }
    if (Op == "insertelt" && NC == 3) {
      std::string A = Child(0), S = Child(1);
      std::string I = immArg(C, N.getChild(2));
      if (A.empty() || S.empty() || I.empty())
        return {};
      return (Twine("LC.builder().CreateInsertElement(") + A +
              ", LC.vcast(" + S + ", " + typeExpr(VT.getVectorElementType()) +
              "), (uint64_t)" + I + ")")
          .str();
    }
    if (Op == "extractelt" && NC == 2) {
      std::string A = Child(0);
      std::string I = immArg(C, N.getChild(1));
      if (A.empty() || I.empty())
        return {};
      return (Twine("LC.vcast(LC.builder().CreateExtractElement(") + A +
              ", (uint64_t)" + I + "), " + Ty + ")")
          .str();
    }
    if ((Op == "insert_subvector" && NC == 3) ||
        (Op == "extract_subvector" && NC == 2)) {
      bool Ins = Op == "insert_subvector";
      const TreePatternNode &Sub = N.getChild(Ins ? 1 : 0);
      if (!hasType(Sub) || !Sub.getSimpleType(0).isVector() ||
          !(Ins ? true : VT.isVector())) {
        C.Failed = true;
        return {};
      }
      unsigned SubElts =
          (Ins ? Sub.getSimpleType(0) : VT).getVectorNumElements();
      std::string A = Child(0), S = Ins ? Child(1) : std::string();
      std::string I = immArg(C, N.getChild(Ins ? 2 : 1), SubElts);
      if (A.empty() || (Ins && S.empty()) || I.empty())
        return {};
      return (Twine("LC.vecOp(\"") + (Ins ? "insert_sub" : "extract_sub") +
              "\", " + Ty + ", {" + A + (Ins ? ", " + S : std::string()) +
              "}, " + I + ")")
          .str();
    }
    if (Op == "sint_to_fp" || Op == "fp_to_sint" || Op == "fp_extend" ||
        Op == "fp_round") {
      const TreePatternNode &Src = N.getChild(0);
      if (NC != 1 || !hasType(Src) || !VT.isVector() ||
          !Src.getSimpleType(0).isVector()) {
        C.Failed = true;
        return {};
      }
      MVT ST = Src.getSimpleType(0);
      std::string A = Child(0);
      if (A.empty())
        return {};
      // Convert the overlapping low lanes; a narrowing result is zero-padded.
      unsigned Lanes = std::min(ST.getVectorNumElements(),
                                VT.getVectorNumElements());
      const char *Cast = Op == "sint_to_fp"   ? "sitofp"
                         : Op == "fp_to_sint" ? "fptosi"
                         : Op == "fp_extend"  ? "fpext"
                                              : "fptrunc";
      return (Twine("LC.vecConvert(\"") + Cast + "\", " + A + ", " + Ty + ", " +
              Twine(Lanes) + ")")
          .str();
    }
    if (const VecKind *K = vecKind(Op)) {
      if (NC != K->Vals + (K->Imm ? 1 : 0)) {
        C.Failed = true;
        return {};
      }
      std::string Args;
      for (unsigned I = 0; I < K->Vals; ++I) {
        std::string A = Child(I);
        if (A.empty())
          return {};
        Args += (I ? ", " : "") + A;
      }
      std::string Imm = "0";
      if (K->Imm) {
        Imm = immArg(C, N.getChild(K->Vals));
        if (Imm.empty())
          return {};
      }
      return (Twine("LC.vecOp(\"") + K->Kind + "\", " + Ty + ", {" + Args +
              "}, " + Imm + ")")
          .str();
    }
    C.Failed = true;
    return {};
  }

  /// Body for a pattern whose value is a vector; empty when the pattern is
  /// not a vector one (`IsVector` false) or cannot be inverted.
  static std::string vectorPatternBody(const PatternToMatch &P,
                                       const CodeGenInstruction &Inst,
                                       bool &IsVector) {
    IsVector = false;
    const TreePatternNode &Dst = P.getDstPattern();
    const TreePatternNode &Src = P.getSrcPattern();
    const TreePatternNode *Expr = &Src;
    if (!Src.isLeaf() && Src.getOperator()->getName() == "set" &&
        Src.getNumChildren() == 2)
      Expr = &Src.getChild(1);
    StringRef SrcOp =
        Expr->isLeaf() ? StringRef() : Expr->getOperator()->getName();
    bool IsStore = SrcOp == "st" || SrcOp == "store";
    const TreePatternNode &Val = IsStore && Expr->getNumChildren() == 2
                                     ? Expr->getChild(0)
                                     : *Expr;
    if (!hasType(Val) || !anyVectorType(Val))
      return {};
    IsVector = true;
    // Mask registers and implicit state belong to the DSL.
    for (unsigned K = 0, E = Inst.Operands.size(); K != E; ++K)
      if (Inst.Operands[K].Rec->getName().starts_with("VK"))
        return {};
    VecPat C{Inst, Dst};
    std::string V = vecExpr(C, Val);
    if (C.Failed || V.empty())
      return {};
    if (IsStore) {
      auto Mem = patOperandNo(C, firstNamedOperand(Expr->getChild(1)));
      if (Inst.Operands.NumDefs != 0 || !Mem || !isMemOperand(Inst, *Mem))
        return {};
      return (Twine("    LC.writeOp(MI, ") + Twine(*Mem) + ", " + V + ");\n")
          .str();
    }
    if (Inst.Operands.NumDefs != 1)
      return {};
    MVT RootVT = Val.getSimpleType(0);
    bool Vex = Inst.TheDef->getName().starts_with("V");
    // VEX.128 writes clear the upper ymm lane; a scalar result (movmsk,
    // extract) goes to a general register.
    bool Zero = Vex && RootVT.isVector() && RootVT.getSizeInBits() <= 128;
    return (Twine("    LC.") + (Zero ? "writeOpZ" : "writeOp") +
            "(MI, 0, " + V + ");\n")
        .str();
  }

  static std::string patternBody(const PatternToMatch &P,
                                const CodeGenInstruction &Inst,
                                unsigned Width) {
    const TreePatternNode &Dst = P.getDstPattern();
    if (Dst.isLeaf() || Dst.getOperator() != Inst.TheDef ||
        hasNestedInst(Dst) || !Inst.ImplicitDefs.empty())
      return {};
    bool IsVector = false;
    std::string VecBody = vectorPatternBody(P, Inst, IsVector);
    if (IsVector)
      return VecBody;
    const TreePatternNode &Src = P.getSrcPattern();
    const TreePatternNode *Expr = &Src;
    if (!Src.isLeaf() && Src.getOperator()->getName() == "set" &&
        Src.getNumChildren() == 2)
      Expr = &Src.getChild(1);
    StringRef SrcOp =
        Expr->isLeaf() ? StringRef() : Expr->getOperator()->getName();
    if (SrcOp == "st" || SrcOp == "store" || SrcOp == "atomic_store") {
      if (Inst.Operands.NumDefs != 0 || Expr->getNumChildren() != 2)
        return {};
      std::string Value = treeExpression(Expr->getChild(0), Inst, Width);
      auto Mem = operandNo(Inst, firstNamedOperand(Expr->getChild(1)));
      return Value.empty() || !Mem
                 ? std::string()
                 : (Twine("    LC.writeOp(MI, ") + Twine(*Mem) + ", " +
                    Value + ");\n")
                       .str();
    }
    if (Inst.Operands.NumDefs != 1)
      return {};
    std::string Value = treeExpression(*Expr, Inst, Width);
    if (Value.empty())
      return {};
    return (Twine("    LC.writeOp(MI, 0, ") + Value + ");\n").str();
  }

  static std::string rmwResult(StringRef Op, StringRef Old, StringRef Src) {
    if (Op == "add") return (Twine("LC.flagsAdd(") + Old + ", " + Src + ")").str();
    if (Op == "sub") return (Twine("LC.flagsSub(") + Old + ", " + Src + ")").str();
    if (Op == "adc") return (Twine("LC.flagsAdc(") + Old + ", " + Src + ")").str();
    if (Op == "sbb") return (Twine("LC.flagsSbb(") + Old + ", " + Src + ")").str();
    if (Op == "and" || Op == "or" || Op == "xor") {
      StringRef Builder = Op == "and" ? "CreateAnd"
                          : Op == "or" ? "CreateOr"
                                       : "CreateXor";
      return (Twine("LC.flagsLogic(LC.builder().") + Builder + "(" + Old +
              ", " + Src + "))")
          .str();
    }
    if (Op == "inc") return (Twine("LC.flagsInc(") + Old + ")").str();
    if (Op == "dec") return (Twine("LC.flagsDec(") + Old + ")").str();
    if (Op == "neg") return (Twine("LC.flagsNeg(") + Old + ")").str();
    if (Op == "not")
      return (Twine("LC.builder().CreateNot(") + Old + ")").str();
    return {};
  }

  static bool emitBody(raw_ostream &OS, const DagInit &Body,
                       const CodeGenInstruction &Inst, unsigned Width) {
    const Record *Op = opOf(Body);
    if (!Op)
      return false;
    StringRef Name = Op->getName();
    if (Name == "rv_seq") {
      for (unsigned I = 0; I < Body.getNumArgs(); ++I) {
        const auto *Child = dyn_cast<DagInit>(Body.getArg(I));
        if (!Child || !emitBody(OS, *Child, Inst, Width))
          return false;
      }
      return true;
    }
    if (Name == "rv_nop")
      return true;
    if (Name == "rv_set" || Name == "rv_setz") {
      if (Body.getNumArgs() != 2)
        return false;
      std::string Value = expression(
          Body.getArg(1), Inst, Width, Body.getArgNameStr(1));
      if (Value.empty())
        return false;
      StringRef DstName = Body.getArgNameStr(0);
      const Init *Dst = Body.getArg(0);

      if (!DstName.empty()) {
        auto N = operandNo(Inst, DstName);
        if (!N)
          return false;
        OS << "    LC." << (Name == "rv_setz" ? "writeOpZ" : "writeOp")
           << "(MI, " << *N << ", " << Value << ");\n";
        return true;
      }
      if (const auto *D = dyn_cast<DagInit>(Dst)) {
        const Record *DstOp = opOf(*D);
        if (DstOp && DstOp->getName() == "rv_reg" &&
            D->getNumArgs() == 1) {
          const auto *Reg = dyn_cast<StringInit>(D->getArg(0));
          if (!Reg)
            return false;
          OS << "    LC.regWrite(X86::" << Reg->getValue() << ", " << Value
             << ");\n";
          return true;
        }
      }
      return false;
    }
    if (Name == "rv_x87push" && Body.getNumArgs() == 1) {
      std::string Value =
          expression(Body.getArg(0), Inst, 80, Body.getArgNameStr(0));
      if (Value.empty())
        return false;
      OS << "    LC.x87Push(" << Value << ");\n";
      return true;
    }
    if (Name == "rv_x87pop") {
      OS << "    (void)LC.x87Pop();\n";
      return true;
    }
    if (Name == "rv_x87stset" && Body.getNumArgs() == 2) {
      std::string Index = x87IndexExpr(Body.getArg(0), Inst);
      std::string Value =
          expression(Body.getArg(1), Inst, 80, Body.getArgNameStr(1));
      if (Index.empty() || Value.empty())
        return false;
      OS << "    LC.x87StSet(" << Index << ", " << Value << ");\n";
      return true;
    }
    if (Name == "rv_x87xchg" && Body.getNumArgs() == 1) {
      std::string Index = x87IndexExpr(Body.getArg(0), Inst);
      if (Index.empty())
        return false;
      OS << "    LC.x87Xchg(" << Index << ");\n";
      return true;
    }
    if (Name == "rv_fcom" || Name == "rv_fcomi") {
      std::string Value = "nullptr";
      if (Body.getNumArgs()) {
        const auto *D = dyn_cast<DagInit>(Body.getArg(0));
        const Record *ArgOp = D ? opOf(*D) : nullptr;
        if (ArgOp && ArgOp->getName() == "rv_x87idx") {
          std::string Index = x87IndexExpr(Body.getArg(0), Inst);
          if (Index.empty())
            return false;
          Value = (Twine("LC.x87St(") + Index + ")").str();
        } else if (Name == "rv_fcom") {
          auto Mem = operandNo(Inst, Body.getArgNameStr(0));
          if (!Mem)
            return false;
          std::string Loaded =
              (Twine("LC.readOp(MI, ") + Twine(*Mem) + ", LC.iN(" +
               Twine(Width) + "))")
                  .str();
          if (StringRef(Inst.TheDef->getName()).starts_with("FICOM"))
            Value = (Twine("LC.builder().CreateSIToFP(") + Loaded +
                     ", LC.fltTy(80))")
                        .str();
          else {
            std::string F = (Twine("LC.builder().CreateBitCast(") + Loaded +
                             ", LC.fltTy(" + Twine(Width) + "))")
                                .str();
            Value = (Twine("LC.builder().CreateFPExt(") + F +
                     ", LC.fltTy(80))")
                        .str();
          }
        } else {
          return false;
        }
      }
      OS << "    LC." << (Name == "rv_fcom" ? "x87FCom" : "x87FComI")
         << "(" << Value << ");\n";
      return true;
    }
    if (Name == "rv_fldcw" && Body.getNumArgs() == 1) {
      auto Mem = operandNo(Inst, Body.getArgNameStr(0));
      if (!Mem)
        return false;
      OS << "    LC.x87SetControlWord(LC.memRead(MI, " << *Mem
         << ", LC.iN(16)));\n";
      return true;
    }
    if (Name == "rv_fstcw" && Body.getNumArgs() == 1) {
      auto Mem = operandNo(Inst, Body.getArgNameStr(0));
      if (!Mem)
        return false;
      OS << "    LC.memWrite(MI, " << *Mem << ", LC.x87ControlWord());\n";
      return true;
    }
    if (Name == "rv_x87init") {
      OS << "    LC.x87Init();\n";
      return true;
    }
    if (Name == "rv_fclex")
      return true;
    if (Name == "rv_fst" && Body.getNumArgs() == 3) {
      const auto *Kind = dyn_cast<StringInit>(Body.getArg(0));
      auto Mem = operandNo(Inst, Body.getArgNameStr(1));
      std::string Value =
          expression(Body.getArg(2), Inst, 80, Body.getArgNameStr(2));
      if (!Kind || !Mem || Value.empty())
        return false;
      StringRef K = Kind->getValue();
      unsigned Bits = K.ends_with("80") ? 80
                      : K.ends_with("64") ? 64
                      : K.ends_with("32") ? 32
                      : K.ends_with("16") ? 16
                                          : 0;
      if (!Bits)
        return false;
      if (K.starts_with("f"))
        OS << "    LC.memWrite(MI, " << *Mem << ", LC.builder().CreateFPTrunc("
           << Value << ", LC.fltTy(" << Bits << ")));\n";
      else
        OS << "    LC.memWrite(MI, " << *Mem
           << ", LC.builder().CreateFPToSI(" << Value << ", LC.iN(" << Bits
           << ")));\n";
      return true;
    }
    if (Name == "rv_bt") {
      std::string Value = expression(&Body, Inst, Width);
      if (Value.empty())
        return false;
      OS << "    (void)" << Value << ";\n";
      return true;
    }

    if (Name == "rv_fcmpf" && Body.getNumArgs() == 2) {
      std::string A =
          expression(Body.getArg(0), Inst, 64, Body.getArgNameStr(0));
      std::string B =
          expression(Body.getArg(1), Inst, 64, Body.getArgNameStr(1));
      if (A.empty() || B.empty())
        return false;
      OS << "    Value *FCmpUno = LC.builder().CreateFCmpUNO(" << A << ", "
         << B << ");\n"
         << "    LC.flagWrite(X86LiftContext::ZF, LC.builder().CreateOr(FCmpUno, LC.builder().CreateFCmpOEQ("
         << A << ", " << B << ")));\n"
         << "    LC.flagWrite(X86LiftContext::PF, FCmpUno);\n"
         << "    LC.flagWrite(X86LiftContext::CF, LC.builder().CreateOr(FCmpUno, LC.builder().CreateFCmpOLT("
         << A << ", " << B << ")));\n"
         << "    LC.flagWrite(X86LiftContext::OF, LC.constI(0, 1));\n"
         << "    LC.flagWrite(X86LiftContext::SF, LC.constI(0, 1));\n"
         << "    LC.flagWrite(X86LiftContext::AF, LC.constI(0, 1));\n";
      return true;
    }
    if (Name == "rv_ptest" && Body.getNumArgs() == 2) {
      std::string A =
          expression(Body.getArg(0), Inst, 128, Body.getArgNameStr(0));
      std::string B =
          expression(Body.getArg(1), Inst, 128, Body.getArgNameStr(1));
      if (A.empty() || B.empty())
        return false;
      OS << "    LC.ptestOp(" << A << ", " << B << ");\n";
      return true;
    }

    if (Name == "rv_memmove" && Body.getNumArgs() == 3) {
      std::string Dst =
          expression(Body.getArg(0), Inst, 64, Body.getArgNameStr(0));
      std::string Src =
          expression(Body.getArg(1), Inst, 64, Body.getArgNameStr(1));
      std::string Len =
          expression(Body.getArg(2), Inst, 64, Body.getArgNameStr(2));
      if (Dst.empty() || Src.empty() || Len.empty())
        return false;
      OS << "    LC.memMove(" << Dst << ", " << Src << ", " << Len << ");\n";
      return true;
    }
    if (Name == "rv_memset" && Body.getNumArgs() == 3) {
      std::string Dst =
          expression(Body.getArg(0), Inst, 64, Body.getArgNameStr(0));
      std::string Value =
          expression(Body.getArg(1), Inst, 8, Body.getArgNameStr(1));
      std::string Len =
          expression(Body.getArg(2), Inst, 64, Body.getArgNameStr(2));
      if (Dst.empty() || Value.empty() || Len.empty())
        return false;
      OS << "    LC.memSet(" << Dst << ", " << Value << ", " << Len << ");\n";
      return true;
    }
    if ((Name == "rv_cmpxchg8b" || Name == "rv_cmpxchg16b") &&
        Body.getNumArgs() == 1) {
      std::string Addr =
          expression(Body.getArg(0), Inst, 64, Body.getArgNameStr(0));
      if (Addr.empty())
        return false;
      OS << "    LC.cmpxchgPair(" << Addr << ", "
         << (Name == "rv_cmpxchg8b" ? 64 : 128) << ");\n";
      return true;
    }
    if (Name == "rv_xchg" && Body.getNumArgs() == 2) {
      auto AOp = operandNo(Inst, Body.getArgNameStr(0));
      auto BOp = operandNo(Inst, Body.getArgNameStr(1));
      if (AOp && BOp) {
        OS << "    LC.xchgOps(MI, " << *AOp << ", " << *BOp << ");\n";
        return true;
      }
      const auto *RegDag = dyn_cast<DagInit>(Body.getArg(1));
      const Record *RegOp = RegDag ? opOf(*RegDag) : nullptr;
      const auto *Reg =
          RegDag && RegDag->getNumArgs() == 1
              ? dyn_cast<StringInit>(RegDag->getArg(0))
              : nullptr;
      if (!AOp || !RegOp || RegOp->getName() != "rv_reg" || !Reg)
        return false;
      OS << "    Value *XchgA = LC.readOp(MI, " << *AOp << ", LC.iN("
         << Width << "));\n"
         << "    Value *XchgB = LC.regRead(X86::" << Reg->getValue()
         << ", LC.iN(" << Width << "));\n"
         << "    LC.writeOp(MI, " << *AOp << ", XchgB);\n"
         << "    LC.regWrite(X86::" << Reg->getValue() << ", XchgA);\n";
      return true;
    }
    if (Name == "rv_xadd" && Body.getNumArgs() == 2) {
      auto Dst = operandNo(Inst, Body.getArgNameStr(0));
      auto Src = operandNo(Inst, Body.getArgNameStr(1));
      if (!Dst || !Src)
        return false;
      if (isMemOperand(Inst, *Dst)) {
        // The register receives the old memory value; with LOCK the add is
        // one atomicrmw and the flags come from old + val.
        OS << "    Value *XAddAddr = LC.memAddr(MI, " << *Dst << ");\n"
           << "    Value *XAddVal = LC.readOp(MI, " << *Src << ", LC.iN("
           << Width << "));\n"
           << "    Value *XAddOld = LC.hasLock() ? LC.atomicRMW(\"add\", "
              "XAddAddr, XAddVal) : LC.memReadAddr(XAddAddr, LC.iN("
           << Width << "));\n"
           << "    Value *XAddSum = LC.flagsAdd(XAddOld, XAddVal);\n"
           << "    if (!LC.hasLock()) LC.memWriteAddr(XAddAddr, XAddSum);\n"
           << "    LC.writeOp(MI, " << *Src << ", XAddOld);\n";
        return true;
      }
      OS << "    Value *XAddA = LC.readOp(MI, " << *Dst << ", LC.iN("
         << Width << "));\n"
         << "    Value *XAddB = LC.readOp(MI, " << *Src << ", LC.iN("
         << Width << "));\n"
         << "    Value *XAddSum = LC.xaddVal(MI, " << *Dst << ", " << *Src
         << ", XAddA, XAddB);\n"
         << "    LC.writeOp(MI, " << *Dst << ", XAddSum);\n";
      return true;
    }
    if (Name == "rv_mulx" && Body.getNumArgs() == 3) {
      // mulx_args operand order is $dst1(hi), $dst2(lo), $src.
      auto Hi = operandNo(Inst, Body.getArgNameStr(0));
      auto Lo = operandNo(Inst, Body.getArgNameStr(1));
      auto Src = operandNo(Inst, Body.getArgNameStr(2));
      if (!Hi || !Lo || !Src)
        return false;
      OS << "    LC.mulxOp(MI, " << *Hi << ", " << *Lo << ", " << *Src
         << ", " << Width << ");\n";
      return true;
    }
    if (Name == "rv_cmpxchgr" && Body.getNumArgs() == 2) {
      auto Dst = operandNo(Inst, Body.getArgNameStr(0));
      auto Src = operandNo(Inst, Body.getArgNameStr(1));
      if (!Dst || !Src)
        return false;
      StringRef Acc = Width == 8 ? "AL" : Width == 16 ? "AX"
                          : Width == 32 ? "EAX" : "RAX";
      OS << "    Value *CmpAcc = LC.regRead(X86::" << Acc << ", LC.iN("
         << Width << "));\n"
         << "    Value *CmpOld = LC.readOp(MI, " << *Dst << ", LC.iN("
         << Width << "));\n"
         << "    Value *CmpNew = LC.readOp(MI, " << *Src << ", LC.iN("
         << Width << "));\n"
         << "    Value *CmpEq = LC.builder().CreateICmpEQ(CmpAcc, CmpOld);\n"
         << "    (void)LC.flagsCmp(CmpAcc, CmpOld);\n"
         << "    LC.regWrite(X86::" << Acc
         << ", LC.builder().CreateSelect(CmpEq, CmpAcc, CmpOld));\n"
         << "    LC.writeOp(MI, " << *Dst
         << ", LC.builder().CreateSelect(CmpEq, CmpNew, CmpOld));\n";
      return true;
    }

    if (Name == "rv_fcmp" || Name == "rv_fand" || Name == "rv_for" ||
        Name == "rv_fxor") {
      std::string Value = expression(&Body, Inst, Width);
      if (Value.empty())
        return false;
      OS << "    (void)" << Value << ";\n";
      return true;
    }
    if (Name == "rv_flagset" && Body.getNumArgs() == 2) {
      const auto *Flag = dyn_cast<StringInit>(Body.getArg(0));
      if (!Flag || flagEnum(Flag->getValue()).empty())
        return false;
      std::string Value = expression(Body.getArg(1), Inst, 1,
                                     Body.getArgNameStr(1));
      if (Value.empty())
        return false;
      OS << "    LC.flagWrite(X86LiftContext::" << flagEnum(Flag->getValue())
         << ", " << Value << ");\n";
      return true;
    }
    if (Name == "rv_sahf") {
      OS << "    LC.sahfOp();\n";
      return true;
    }
    if (Name == "rv_lahf")
      return false;
    if (Name == "rv_pushf") {
      OS << "    LC.pushFlagsVal();\n";
      return true;
    }
    if (Name == "rv_popf") {
      OS << "    LC.popFlagsVal();\n";
      return true;
    }
    if (Name == "rv_leave") {
      OS << "    LC.leaveOp();\n";
      return true;
    }
    if (Name == "rv_fence") {
      OS << "    LC.emitFence();\n";
      return true;
    }
    if (Name == "rv_sextacc" && Body.getNumArgs() == 1) {
      const auto *W = dyn_cast<IntInit>(Body.getArg(0));
      if (!W)
        return false;
      int64_t Value = W->getValue();
      // sextAcc takes 100 + W for the CWD/CDQ/CQO family that fills the
      // high half.
      unsigned Width = Value < 0 ? 100 + static_cast<unsigned>(-Value)
                                 : static_cast<unsigned>(Value);
      OS << "    LC.sextAcc(" << Width << ");\n";
      return true;
    }
    if (Name == "rv_frmw" && Body.getNumArgs() == 3) {
      const auto *OpName = dyn_cast<StringInit>(Body.getArg(0));
      auto Mem = operandNo(Inst, Body.getArgNameStr(1));
      std::string Src =
          expression(Body.getArg(2), Inst, Width, Body.getArgNameStr(2));
      if (!OpName || !Mem || Src.empty())
        return false;
      StringRef OpText = OpName->getValue();
      if (OpText == "neg") {
        OS << "    if (LC.hasLock()) { LC.emitOpaque(MI, \""
           << Inst.TheDef->getName() << "\"); return true; }\n";
      }
      OS << "    Value *RMWAddr = LC.memAddr(MI, " << *Mem << ");\n";
      OS << "    Value *RMWValue = " << Src << ";\n"
            "    Value *RMWFlagSrc = RMWValue;\n";
      if (OpText == "not")
        OS << "    RMWValue = LC.constI(-1, " << Width << ");\n";
      else if (OpText == "inc" || OpText == "dec")
        OS << "    RMWValue = LC.constI(1, " << Width << ");\n";
      else if (OpText == "adc" || OpText == "sbb")
        OS << "    RMWValue = LC.builder().CreateAdd(RMWValue, "
              "LC.builder().CreateZExt(LC.flagRead(X86LiftContext::CF), "
           << "LC.iN(" << Width << ")));\n";
      OS << "    Value *RMWOld = LC.hasLock() ? LC.atomicRMW(\""
         << (OpText == "inc" ? "add" : OpText == "dec" ? "sub"
               : OpText == "adc" ? "add" : OpText == "sbb" ? "sub"
               : OpText == "not" ? "xor" : OpText)
         << "\", RMWAddr, RMWValue) : "
            "LC.memReadAddr(RMWAddr, LC.iN("
         << Width << "));\n";
      std::string Result = rmwResult(OpText, "RMWOld", "RMWFlagSrc");
      if (Result.empty())
        return false;
      OS << "    Value *RMWResult = " << Result << ";\n"
            "    if (!LC.hasLock()) LC.memWriteAddr(RMWAddr, RMWResult);\n";
      return true;
    }

    if ((Name == "rv_push" || Name == "rv_mulpair" ||
         Name == "rv_divpair") &&
        Body.getNumArgs() >= 2) {
      unsigned WidthArg = Name == "rv_push" ? 0 : 1;
      unsigned ValueArg = Name == "rv_push" ? 1 : 2;
      if (Name == "rv_push") {
        const auto *W = dyn_cast<IntInit>(Body.getArg(WidthArg));
        if (!W)
          return false;
        std::string Value =
            expression(Body.getArg(ValueArg), Inst, W->getValue(),
                       Body.getArgNameStr(ValueArg));
        if (Value.empty())
          return false;
        OS << "    LC.pushVal(" << Value << ");\n";
        return true;
      }
      const auto *Sign = dyn_cast<StringInit>(Body.getArg(0));
      const auto *W = dyn_cast<IntInit>(Body.getArg(1));
      if (!Sign || !W)
        return false;
      std::string Value = expression(Body.getArg(2), Inst, W->getValue(),
                                     Body.getArgNameStr(2));
      if (Value.empty())
        return false;
      OS << "    LC." << (Name == "rv_mulpair" ? "mulPair" : "divPair")
         << "(" << Value << ", " << W->getValue() << ", "
         << (Sign->getValue() == "s" ? "true" : "false") << ");\n";
      return true;
    }
    if (Name == "rv_brcc" && Body.getNumArgs() == 2) {
      auto Dst = operandNo(Inst, Body.getArgNameStr(0));
      auto Cond = operandNo(Inst, Body.getArgNameStr(1));
      if (!Dst || !Cond)
        return false;
      OS << "    LC.brccOp(MI, " << *Dst << ", " << *Cond << ");\n";
      return true;
    }
    if (Name == "rv_brcond" && Body.getNumArgs() == 2) {
      auto Dst = operandNo(Inst, Body.getArgNameStr(0));
      std::string Pred =
          expression(Body.getArg(1), Inst, 1, Body.getArgNameStr(1));
      if (!Dst || Pred.empty())
        return false;
      OS << "    LC.condJump(" << Pred << ", LC.pcrelTarget(MI, " << *Dst
         << "));\n";
      return true;
    }

    if ((Name == "rv_br" || Name == "rv_callr" || Name == "rv_calli") &&
        Body.getNumArgs() == 1) {
      auto N = operandNo(Inst, Body.getArgNameStr(0));
      if (!N)
        return false;
      if (Name == "rv_br")
        OS << "    LC.brOp(MI, " << *N << ");\n";
      else
        OS << "    LC.callToOp(MI, " << *N << ");\n";
      return true;
    }
    if (Name == "rv_ret") {
      OS << "    LC.retOp();\n";
      return true;
    }
    if (Name == "rv_prefix" && Body.getNumArgs() >= 1) {
      const auto *Kind = dyn_cast<StringInit>(Body.getArg(0));
      if (!Kind)
        return false;
      if (Body.getNumArgs() == 1) {
        OS << "    LC.prefixOp(\"" << Kind->getValue() << "\");\n";
        return true;
      }
      const auto *Reg = dyn_cast<DagInit>(Body.getArg(1));
      const Record *RegOp = Reg ? opOf(*Reg) : nullptr;
      const auto *RegName =
          Reg && Reg->getNumArgs() == 1
              ? dyn_cast<StringInit>(Reg->getArg(0))
              : nullptr;
      if (!RegOp || RegOp->getName() != "rv_reg" || !RegName)
        return false;
      OS << "    LC.prefixOp(\"" << Kind->getValue() << "\", X86::"
         << RegName->getValue() << ");\n";
      return true;
    }
    if (Name == "rv_store" && Body.getNumArgs() == 2) {
      std::string Addr =
          expression(Body.getArg(0), Inst, 64, Body.getArgNameStr(0));
      std::string Value =
          expression(Body.getArg(1), Inst, Width, Body.getArgNameStr(1));
      if (Addr.empty() || Value.empty())
        return false;
      OS << "    LC.memWriteAddr(" << Addr << ", " << Value << ");\n";
      return true;
    }
    // (rv_int N | (rv_op K) | $op): the imm8 interrupt vector, a literal or
    // the instruction's immediate operand.
    if (Name == "rv_int" && Body.getNumArgs() == 1) {
      std::string Imm;
      if (auto OpNo = operandNo(Inst, Body.getArgNameStr(0)))
        Imm = (Twine("MI.getOperand(") + Twine(*OpNo) + ").getImm()").str();
      else
        Imm = immediateExpr(Body.getArg(0), Inst);
      if (Imm.empty())
        return false;
      OS << "    LC.intTrap(" << Imm << ");\n";
      return true;
    }
    if (Name == "rv_opaque") {
      OS << "    LC.emitOpaque(MI, \"" << Inst.TheDef->getName() << "\");\n";
      return true;
    }
    return false;
  }

  /// Every `MI, N` and `MI.getOperand(N)` in a generated body must name the
  /// first MCInst operand of one of the instruction's operands, and the
  /// memory accessors must name a five-operand address group. A DSL glob
  /// written for the register form that also matches the memory form fails
  /// this check; the case then falls back to a pattern or the opaque path
  /// instead of reading a base register or scale as data.
  static bool operandRefsValid(StringRef Code, const CodeGenInstruction &Inst) {
    if (Inst.Operands.isVariadic)
      return true;
    auto Start = [&](unsigned N, bool NeedMem) {
      for (unsigned I = 0, E = Inst.Operands.size(); I != E; ++I) {
        const auto &Op = Inst.Operands[I];
        if (Op.MIOperandNo == N)
          return !NeedMem || Op.MINumOperands == 5;
      }
      return false;
    };
    auto Number = [&](size_t Pos, unsigned &N) {
      size_t End = Pos;
      while (End < Code.size() && isDigit(Code[End]))
        ++End;
      return End != Pos &&
             !Code.substr(Pos, End - Pos).getAsInteger(10, N);
    };
    for (size_t Pos = Code.find("(MI, "); Pos != StringRef::npos;
         Pos = Code.find("(MI, ", Pos + 1)) {
      size_t NameEnd = Pos;
      size_t NameBegin = NameEnd;
      while (NameBegin > 0 &&
             (isAlnum(Code[NameBegin - 1]) || Code[NameBegin - 1] == '_'))
        --NameBegin;
      StringRef Fn = Code.substr(NameBegin, NameEnd - NameBegin);
      unsigned N;
      if (!Number(Pos + 5, N))
        continue;
      bool NeedMem = Fn == "memRead" || Fn == "memWrite" || Fn == "memAddr";
      if (!Start(N, NeedMem))
        return false;
    }
    for (size_t Pos = Code.find("MI.getOperand("); Pos != StringRef::npos;
         Pos = Code.find("MI.getOperand(", Pos + 1)) {
      unsigned N;
      if (Number(Pos + 14, N) && !Start(N, false))
        return false;
    }
    return true;
  }

  static bool globMatches(StringRef Pattern, StringRef Name) {
    size_t Star = Pattern.find('*');
    if (Star == StringRef::npos)
      return Pattern == Name;
    StringRef Prefix = Pattern.take_front(Star);
    StringRef Suffix = Pattern.drop_front(Star + 1);
    return Name.starts_with(Prefix) && Name.ends_with(Suffix) &&
           Name.size() >= Prefix.size() + Suffix.size();
  }

public:
  explicit RevenantLiftEmitter(const RecordKeeper &R)
      : Records(R), Patterns(R) {}

  void run(raw_ostream &OS) {
    const CodeGenTarget &Target = Patterns.getTargetInfo();
    std::map<const Record *, const CodeGenInstruction *> Instructions;
    for (const CodeGenInstruction *I : Target.getTargetInstructions())
      Instructions[I->TheDef] = I;
    std::map<const Record *, const DagInit *> Semantics;
    for (const Record *R : Records.getAllDerivedDefinitions("RVLift")) {
      const Record *Insn = R->getValueAsDef("Insn");
      if (Instructions.count(Insn))
        Semantics[Insn] = bodyOf(*R);
    }
    // An exact name beats a wildcard, so `MOV*rm` (the general-register
    // moves) cannot capture MOVSDrm and friends.
    for (bool Wild : {false, true})
      for (const Record *R : Records.getAllDerivedDefinitions("RVMatch")) {
        StringRef Glob = R->getValueAsString("Glob");
        if (Glob.contains('*') != Wild)
          continue;
        const DagInit *Body = bodyOf(*R);
        for (const auto &I : Instructions)
          if (!Semantics.count(I.first) &&
              globMatches(Glob, I.first->getName()))
            Semantics[I.first] = Body;
      }
    std::map<const Record *, std::string> PatternBodies;
    for (const PatternToMatch &P : Patterns.ptms()) {
      const TreePatternNode &Dst = P.getDstPattern();
      if (Dst.isLeaf() || hasNestedInst(Dst))
        continue;
      const Record *Insn = Dst.getOperator();
      auto It = Instructions.find(Insn);
      if (It == Instructions.end())
        continue;
      const CodeGenInstruction &Inst = *It->second;
      if (!Inst.ImplicitDefs.empty())
        continue;
      std::string Body = patternBody(P, Inst, widthFor(*Insn));
      if (!Body.empty())
        PatternBodies.try_emplace(Insn, std::move(Body));
    }



    OS << "// Generated by revenant-tblgen. Do not edit.\n"
          "#include \"X86MCTargetDesc.h\"\n"
          "#include \"revenant/Lift/X86LiftContext.h\"\n"
          "#include \"llvm/IR/Intrinsics.h\"\n"
          "namespace llvm { namespace revenant {\n"
          "bool liftX86Inst(X86LiftContext &LC, const MCInst &MI, "
          "uint64_t VA, uint64_t Size) {\n"
          "  LC.setVA(VA); LC.setNextVA(VA + Size);\n"
          "  switch (MI.getOpcode()) {\n";

    std::set<const Record *> Emitted;
    std::vector<std::string> Rejected;
    std::vector<std::string> Failed;
    unsigned DSLCount = 0;
    unsigned PatternCount = 0;

    for (const auto &Entry : Semantics) {
      const Record *R = Entry.first;
      const DagInit *Body = Entry.second;
      const CodeGenInstruction &Inst = *Instructions[R];
      std::string Buffer;
      raw_string_ostream BodyOS(Buffer);
      unsigned VW = vectorWidthOf(Inst);
      bool Emits = emitBody(BodyOS, *Body, Inst, VW ? VW : widthFor(*R));
      BodyOS.flush();
      if (Emits && !operandRefsValid(Buffer, Inst)) {
        Rejected.push_back(R->getName().str());
        Emits = false;
      }
      if (Emits) {
        OS << "  case X86::" << R->getName() << ": {\n" << Buffer
           << "    return true;\n  }\n";
        Emitted.insert(R);
        ++DSLCount;
        continue;
      }
      auto Pattern = PatternBodies.find(R);
      if (Pattern == PatternBodies.end()) {
        Failed.push_back(R->getName().str());
        continue;
      }
      OS << "  case X86::" << R->getName() << ": {\n" << Pattern->second
         << "    return true;\n  }\n";
      Emitted.insert(R);
      ++PatternCount;
    }
    for (const auto &Entry : PatternBodies)
      if (!Emitted.count(Entry.first) && !Semantics.count(Entry.first)) {
        OS << "  case X86::" << Entry.first->getName() << ": {\n"
           << Entry.second << "    return true;\n  }\n";
        Emitted.insert(Entry.first);
        ++PatternCount;
      }
    for (const auto &Entry : Instructions)
      if (!Emitted.count(Entry.first)) {
        OS << "  case X86::" << Entry.first->getName() << ": {\n"
           << "    LC.emitOpaque(MI, \"" << Entry.first->getName()
           << "\");\n"
           << "    return true;\n  }\n";
        Emitted.insert(Entry.first);
      }
    unsigned OpaqueCount = Instructions.size() - DSLCount - PatternCount;

    OS << "// Lift coverage: " << Emitted.size() << "/" << Instructions.size()
       << " instructions handled, " << DSLCount << " DSL, " << PatternCount
       << " direct patterns, " << OpaqueCount << " opaque.\n";
    OS << "// Rejected for operand references that miss an operand start: "
       << Rejected.size() << "\n";
    for (const std::string &Name : Rejected)
      OS << "//   " << Name << "\n";
    OS << "// DSL cases the emitter could not lower: " << Failed.size()
       << "\n";
    for (const std::string &Name : Failed)
      OS << "//   " << Name << "\n";


    OS << "  default: return false;\n"
          "  }\n}\n} } // namespace llvm::revenant\n";
  }
};

} // namespace

static TableGen::Emitter::OptClass<RevenantLiftEmitter>
    X("gen-revenant-lift", "Emit Revenant instruction lifting dispatch");
