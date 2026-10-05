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
  std::set<const Record *, LessRecordByID> InstDefs;
  for (const CodeGenInstruction *I : AllInsts)
    InstDefs.insert(I->TheDef);

  // One pass over all patterns: a pattern is direct for instruction I when
  // the Dst root is I and every root child is a leaf. Any instruction record
  // used as an operator anywhere in a Dst tree counts as appearing there.
  for (const PatternToMatch &P : CGP.ptms()) {
    TreePatternNode &Dst = P.getDstPattern();
    if (Dst.isLeaf())
      continue;

    std::set<const Record *> InDst;
    collectDstInsts(Dst, InDst);
    for (const Record *R : InDst)
      if (InstDefs.count(R))
        AppearsInDst.insert(R);

    const Record *Root = Dst.getOperator();
    if (!InstDefs.count(Root))
      continue;
    bool AllLeaves = true;
    for (const TreePatternNode &Child : Dst.children())
      AllLeaves &= Child.isLeaf();
    if (AllLeaves)
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

    std::set<const Record *> CoveredRegs;
    std::vector<std::string> SrcStrs;
    if (Pats) {
      for (const PatternToMatch *P : *Pats) {
        for (const Record *R : P->getDstRegs())
          CoveredRegs.insert(R);
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
    std::vector<std::string> Uncovered;
    for (const Record *R : I->ImplicitDefs) {
      ImplicitDefNames.push_back(R->getName().str());
      if (Pats && !CoveredRegs.count(R))
        Uncovered.push_back(R->getName().str());
    }

    StringRef Category;
    if (Pats)
      Category = Uncovered.empty() ? "full" : "partial";
    else if (AppearsInDst.count(TheDef))
      Category = "composite";
    else
      Category = "none";

    json::Object Entry;
    Entry["name"] = TheDef->getName();
    Entry["decodable"] = Decodable;
    Entry["category"] = Category;
    Entry["uncovered_implicit_defs"] = json::Array(Uncovered);
    Entry["implicit_defs"] = json::Array(ImplicitDefNames);
    Entry["direct_patterns"] = json::Array(SrcStrs);
    Entry["nodes"] = json::Array(
        std::vector<std::string>(NodeNames.begin(), NodeNames.end()));
    Entry["intrinsic_only"] =
        HasIntrinsic && !HasNonGenericNonIntrinsicNonComplex;
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
