//===- RevenantCoverageEmitter.cpp - ISel pattern coverage emitter --------===//
//
// Target-agnostic emitter: reads whichever target .td it is given and reports,
// per instruction, how much of its semantics SelectionDAG ISel patterns
// already describe. Contains no target names; only generic TableGen classes
// (Instruction, SDNode, ComplexPattern, Intrinsic) are referenced.
//
//===----------------------------------------------------------------------===//

#include "Basic/CodeGenIntrinsics.h"
#include "Common/CodeGenDAGPatterns.h"
#include "Common/CodeGenInstruction.h"
#include "Common/CodeGenTarget.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/JSON.h"
#include "llvm/TableGen/Record.h"
#include "llvm/TableGen/TableGenBackend.h"
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace llvm;

namespace {

struct NodeEntry {
  std::string Kind;
  std::string Opcode;
  std::set<const Record *> Instructions;
};

/// Classify one Src-tree node and recurse into children. SDNode operators are
/// "generic" when their Opcode starts with "ISD::", "target" otherwise.
/// ComplexPatterns (which appear as leaves in Src trees) are "complex".
/// Intrinsic wrapper nodes record the intrinsic record as a separate
/// "intrinsic" node in addition to the ISD::INTRINSIC_* wrapper itself.
static void collectSrcNodes(const TreePatternNode &N, const Record *ForInst,
                            const CodeGenDAGPatterns &CGP,
                            std::map<std::string, NodeEntry> &Nodes) {
  if (N.isLeaf()) {
    if (N.getComplexPatternInfo(CGP))
      if (const DefInit *DI = dyn_cast<DefInit>(N.getLeafValue())) {
        NodeEntry &NE = Nodes[DI->getDef()->getName().str()];
        NE.Kind = "complex";
        NE.Instructions.insert(ForInst);
      }
    return;
  }

  const Record *Op = N.getOperator();
  NodeEntry &NE = Nodes[Op->getName().str()];
  NE.Instructions.insert(ForInst);

  if (Op->isSubClassOf("SDNode")) {
    NE.Opcode = CGP.getSDNodeInfo(Op).getEnumName().str();
    NE.Kind = StringRef(NE.Opcode).starts_with("ISD::") ? "generic" : "target";
    if (const CodeGenIntrinsic *Int = N.getIntrinsicInfo(CGP)) {
      NodeEntry &IntNE = Nodes[Int->TheDef->getName().str()];
      IntNE.Kind = "intrinsic";
      IntNE.Instructions.insert(ForInst);
    }
  } else if (N.getComplexPatternInfo(CGP)) {
    NE.Kind = "complex";
  } else {
    NE.Kind = "other";
    ArrayRef<std::pair<const Record *, SMRange>> Supers =
        Op->getDirectSuperClasses();
    if (!Supers.empty())
      NE.Opcode = Supers.front().first->getName().str();
  }

  for (const TreePatternNode &Child : N.children())
    collectSrcNodes(Child, ForInst, CGP, Nodes);
}

/// Collect every Instruction record used as an operator anywhere in a Dst
/// tree, including a non-leaf root and nested children.
static void collectDstInsts(const TreePatternNode &N,
                            std::set<const Record *> &Out) {
  if (N.isLeaf())
    return;
  const Record *Op = N.getOperator();
  if (Op->isSubClassOf("Instruction"))
    Out.insert(Op);
  for (const TreePatternNode &Child : N.children())
    collectDstInsts(Child, Out);
}
static bool operandsCompatible(const CGIOperandList::OperandInfo &A,
                               const CGIOperandList::OperandInfo &B) {
  if (A.Rec == B.Rec)
    return true;
  // OperandType is namespace-prefixed, e.g. "MCOI::OPERAND_IMMEDIATE".
  auto Is = [](StringRef T, StringRef Kind) { return T.ends_with(Kind); };
  return (Is(A.OperandType, "OPERAND_IMMEDIATE") &&
          Is(B.OperandType, "OPERAND_IMMEDIATE")) ||
         (Is(A.OperandType, "OPERAND_PCREL") &&
          Is(B.OperandType, "OPERAND_PCREL"));
}

/// True if any node strictly below N has an Instruction-record operator.
static bool hasNestedInstOp(const TreePatternNode &N) {
  for (const TreePatternNode &Child : N.children()) {
    if (Child.isLeaf())
      continue;
    if (Child.getOperator()->isSubClassOf("Instruction"))
      return true;
    if (hasNestedInstOp(Child))
      return true;
  }
  return false;
}

class RevenantCoverageEmitter {
  CodeGenDAGPatterns CGP;

public:
  RevenantCoverageEmitter(const RecordKeeper &RK) : CGP(RK) {}
  void run(raw_ostream &OS);
};

void RevenantCoverageEmitter::run(raw_ostream &OS) {
  const CodeGenTarget &Target = CGP.getTargetInfo();
  ArrayRef<const CodeGenInstruction *> AllInsts =
      Target.getTargetNonPseudoInstructions();

  std::map<const Record *, std::vector<const PatternToMatch *>,
           LessRecordByID>
      Direct;
  std::set<const Record *> AppearsInDst;
  std::map<const Record *, const CodeGenInstruction *, LessRecordByID> InstMap;
  for (const CodeGenInstruction *I : AllInsts)
    InstMap[I->TheDef] = I;

  // One pass over all patterns: a pattern is direct for instruction I when
  // the Dst root operator is I and no node strictly below the root has an
  // Instruction-record operator. Any instruction record used as an operator
  // anywhere in a Dst tree counts as appearing there.
  for (const PatternToMatch &P : CGP.ptms()) {
    TreePatternNode &Dst = P.getDstPattern();
    if (Dst.isLeaf())
      continue;

    std::set<const Record *> InDst;
    collectDstInsts(Dst, InDst);
    for (const Record *R : InDst)
      if (InstMap.count(R))
        AppearsInDst.insert(R);

    const Record *Root = Dst.getOperator();
    if (!InstMap.count(Root) || hasNestedInstOp(Dst))
      continue;
    Direct[Root].push_back(&P);
  }

  // Collect Src-tree nodes used by each instruction's direct patterns.
  std::map<std::string, NodeEntry> Nodes;
  for (const auto &[Def, Pats] : Direct)
    for (const PatternToMatch *P : Pats)
      collectSrcNodes(P->getSrcPattern(), Def, CGP, Nodes);

  std::vector<const CodeGenInstruction *> Sorted(AllInsts.begin(),
                                                 AllInsts.end());
  llvm::sort(Sorted, [](const CodeGenInstruction *A,
                        const CodeGenInstruction *B) {
    return A->TheDef->getName() < B->TheDef->getName();
  });

  // Pass 1: categories without twins (full/partial/control/move/composite/none).
  std::map<const Record *, std::string, LessRecordByID> Cat;
  std::map<const Record *, std::vector<std::string>, LessRecordByID> Uncovered;
  std::map<const Record *, std::set<const Record *>, LessRecordByID>
      CoveredRegs;
  for (const CodeGenInstruction *I : Sorted) {
    const Record *TheDef = I->TheDef;
    std::set<const Record *> &Covered = CoveredRegs[TheDef];
    auto It = Direct.find(TheDef);
    if (It != Direct.end()) {
      for (const PatternToMatch *P : It->second) {
        for (const Record *R : P->getDstRegs())
          Covered.insert(R);
        // Extra Src root results map to implicit defs in order.
        int64_t K = static_cast<int64_t>(P->getSrcPattern().getNumTypes()) -
                    I->Operands.NumDefs;
        for (int64_t Idx = 0; Idx < K &&
             Idx < static_cast<int64_t>(I->ImplicitDefs.size());
             ++Idx)
          Covered.insert(I->ImplicitDefs[Idx]);
      }
      std::vector<std::string> Miss;
      for (const Record *R : I->ImplicitDefs)
        if (!Covered.count(R))
          Miss.push_back(R->getName().str());
      Uncovered[TheDef] = Miss;
      Cat[TheDef] = Miss.empty() ? "full" : "partial";
      continue;
    }
    if (I->TheDef->getValueAsBit("isBranch") ||
        I->TheDef->getValueAsBit("isCall") ||
        I->TheDef->getValueAsBit("isReturn") ||
        I->TheDef->getValueAsBit("isIndirectBranch")) {
      Cat[TheDef] = "control";
      continue;
    }
    if (I->TheDef->getValueAsBit("isMoveReg")) {
      Cat[TheDef] = "move";
      continue;
    }
    Cat[TheDef] = AppearsInDst.count(TheDef) ? "composite" : "none";
  }

  // Pass 2: twins. Per precedence twin > control > move > composite > none,
  // every instruction without a direct pattern is a twin candidate. Bucket
  // potential twins by (AsmString, NumDefs, operand count) so we don't do an
  // O(N^2) scan over all instructions.
  std::map<std::tuple<std::string, unsigned, unsigned>,
           std::vector<const CodeGenInstruction *>>
      TwinSources;
  for (const CodeGenInstruction *J : Sorted) {
    const std::string &JCat = Cat[J->TheDef];
    if (JCat != "full" && JCat != "partial" && JCat != "control" &&
        JCat != "move")
      continue;
    TwinSources[{J->AsmString.str(), J->Operands.NumDefs,
                 J->Operands.size()}]
        .push_back(J);
  }
  std::map<const Record *, std::vector<const CodeGenInstruction *>,
           LessRecordByID>
      Twins;
  for (const CodeGenInstruction *I : Sorted) {
    const std::string &ICat = Cat[I->TheDef];
    if (ICat == "full" || ICat == "partial")
      continue;
    auto Bit = TwinSources.find(
        {I->AsmString.str(), I->Operands.NumDefs, I->Operands.size()});
    if (Bit == TwinSources.end())
      continue;
    for (const CodeGenInstruction *J : Bit->second) {
      if (J == I)
        continue;
      const CGIOperandList &IOps = I->Operands, &JOps = J->Operands;
      bool AllOk = true;
      for (unsigned O = 0; O < IOps.size(); ++O)
        if (!operandsCompatible(IOps[O], JOps[O])) {
          AllOk = false;
          break;
        }
      if (AllOk)
        Twins[I->TheDef].push_back(J);
    }
  }

  json::Array InstArray;
  for (const CodeGenInstruction *I : Sorted) {
    const Record *TheDef = I->TheDef;
    const std::vector<const PatternToMatch *> *Pats = nullptr;
    if (auto It = Direct.find(TheDef); It != Direct.end())
      Pats = &It->second;

    bool ForceDisassemble = false;
    if (TheDef->getValue("ForceDisassemble")) {
      bool Unset = false;
      ForceDisassemble = TheDef->getValueAsBitOrUnset("ForceDisassemble", Unset);
      ForceDisassemble &= !Unset;
    }
    bool Decodable = !I->isPseudo &&
                     !TheDef->getValueAsBit("isAsmParserOnly") &&
                     (!I->isCodeGenOnly || ForceDisassemble);

    std::vector<std::string> SrcStrs;
    if (Pats) {
      for (const PatternToMatch *P : *Pats) {
        std::string S;
        raw_string_ostream SS(S);
        P->getSrcPattern().print(SS);
        SrcStrs.push_back(SS.str());
      }
    }

    std::set<std::string> NodeNames;
    bool HasIntrinsic = false;
    bool HasNonGenericNonIntrinsicNonComplex = false;
    for (const auto &[Name, NE] : Nodes) {
      if (!NE.Instructions.count(TheDef))
        continue;
      NodeNames.insert(Name);
      if (NE.Kind == "intrinsic")
        HasIntrinsic = true;
      else if (NE.Kind != "generic" && NE.Kind != "complex")
        HasNonGenericNonIntrinsicNonComplex = true;
    }

    std::vector<std::string> ImplicitDefNames;
    for (const Record *R : I->ImplicitDefs)
      ImplicitDefNames.push_back(R->getName().str());

    std::vector<std::string> Flags;
    for (const char *F : {"isBranch", "isCall", "isReturn",
                          "isIndirectBranch", "isMoveReg"})
      if (TheDef->getValueAsBit(F))
        Flags.push_back(F);

    json::Object Entry;
    Entry["name"] = TheDef->getName();
    Entry["decodable"] = Decodable;
    Entry["uncovered_implicit_defs"] = json::Array(Uncovered[TheDef]);
    Entry["implicit_defs"] = json::Array(ImplicitDefNames);
    Entry["direct_patterns"] = json::Array(SrcStrs);
    Entry["flags"] = json::Array(Flags);

    auto TwinIt = Twins.find(TheDef);
    if (TwinIt != Twins.end()) {
      // Prefer a twin with all-identical operand records, then the
      // lexicographically smallest name.
      const CodeGenInstruction *Best = nullptr;
      bool BestAllIdentical = false;
      for (const CodeGenInstruction *J : TwinIt->second) {
        bool AllIdentical = true;
        for (unsigned O = 0; O < I->Operands.size(); ++O)
          if (I->Operands[O].Rec != J->Operands[O].Rec) {
            AllIdentical = false;
            break;
          }
        if (!Best || (AllIdentical && !BestAllIdentical) ||
            (AllIdentical == BestAllIdentical &&
             J->TheDef->getName() < Best->TheDef->getName())) {
          Best = J;
          BestAllIdentical = AllIdentical;
        }
      }
      Entry["category"] = "twin";
      Entry["twin_of"] = Best->TheDef->getName();
      Entry["twin_category"] = Cat[Best->TheDef];
      Entry["twin_candidates"] =
          static_cast<int64_t>(TwinIt->second.size());
      // Copy the twin's semantics onto I.
      std::set<std::string> TwinNodes;
      for (const auto &[Name, NE] : Nodes)
        if (NE.Instructions.count(Best->TheDef))
          TwinNodes.insert(Name);
      std::vector<std::string> TwinPats;
      if (auto Jt = Direct.find(Best->TheDef); Jt != Direct.end())
        for (const PatternToMatch *P : Jt->second) {
          std::string S;
          raw_string_ostream SS(S);
          P->getSrcPattern().print(SS);
          TwinPats.push_back(SS.str());
        }
      bool TwinIntrinsic = false, TwinBadNode = false;
      for (const auto &[Name, NE] : Nodes) {
        if (!NE.Instructions.count(Best->TheDef))
          continue;
        if (NE.Kind == "intrinsic")
          TwinIntrinsic = true;
        else if (NE.Kind != "generic" && NE.Kind != "complex")
          TwinBadNode = true;
      }
      Entry["nodes"] = json::Array(
          std::vector<std::string>(TwinNodes.begin(), TwinNodes.end()));
      Entry["direct_patterns"] = json::Array(TwinPats);
      Entry["intrinsic_only"] = TwinIntrinsic && !TwinBadNode;
      Entry["uncovered_implicit_defs"] =
          json::Array(Uncovered[Best->TheDef]);
    } else {
      Entry["category"] = Cat[TheDef];
      Entry["nodes"] = json::Array(
          std::vector<std::string>(NodeNames.begin(), NodeNames.end()));
      Entry["intrinsic_only"] =
          HasIntrinsic && !HasNonGenericNonIntrinsicNonComplex;
    }
    InstArray.push_back(std::move(Entry));
  }

  // Nodes map is already keyed and ordered by name.
  json::Array NodeArray;
  for (const auto &[Name, NE] : Nodes) {
    json::Object N;
    N["name"] = Name;
    N["kind"] = NE.Kind;
    if (!NE.Opcode.empty())
      N["opcode"] = NE.Opcode;
    N["instructions"] = static_cast<int64_t>(NE.Instructions.size());
    NodeArray.push_back(std::move(N));
  }

  json::Object Root;
  Root["llvm_version"] = LLVM_VERSION_STRING;
  Root["instructions"] = std::move(InstArray);
  Root["nodes"] = std::move(NodeArray);
  OS << json::Value(std::move(Root)) << "\n";
}

} // end anonymous namespace

static TableGen::Emitter::OptClass<RevenantCoverageEmitter>
    X("gen-revenant-coverage",
      "Emit ISel pattern coverage for instruction semantics (Revenant)");
