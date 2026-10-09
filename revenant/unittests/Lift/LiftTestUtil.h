//===- unittests/Lift/LiftTestUtil.h - shared lift test builders ------------===//
//
// Hand-built Program/FuncRecord pieces the lift tests share: an empty
// program over an image, and a function made of one straight block.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_UNITTESTS_LIFT_LIFTTESTUTIL_H
#define REVENANT_UNITTESTS_LIFT_LIFTTESTUTIL_H

#include "revenant/Core/Image.h"
#include "revenant/Core/Instr.h"
#include "revenant/Core/Program.h"
#include "revenant/Disasm/Disasm.h"

#include <map>
#include <memory>
#include <vector>

namespace revenant::lifttest {

/// One function consisting of `Block`, a single block that ends in a return.
inline FuncRecord singleBlockFunc(const Program &P,
                                  const std::vector<Instr> &Block) {
  FuncRecord R;
  R.Start = Block.front().Addr;
  R.End = Block.back().nextVa();
  auto RvaOf = [&](Va V) -> Rva { return *P.Img->rvaOf(V); };
  for (const Instr &I : Block)
    R.InstrRvas.push_back(RvaOf(I.Addr));
  for (uint32_t K = 0; K < Block.size(); ++K)
    R.Sorted.push_back(K);
  BlockRec BR;
  BR.Start = RvaOf(Block.front().Addr);
  BR.End = RvaOf(Block.back().nextVa());
  BR.InstrsBegin = 0;
  BR.InstrsEnd = (uint32_t)Block.size();
  BR.EdgesBegin = 0;
  BR.EdgesEnd = 0;
  BR.EndKind = BlockEnd::Return;
  R.Blocks.push_back(BR);
  R.Chunks.push_back({R.Start, R.End});
  return R;
}

/// A program with no functions and empty name tables over `Img`.
inline Program emptyProgram(std::shared_ptr<Image> Img) {
  Program P;
  P.Img = std::move(Img);
  P.Strings = std::make_shared<std::map<Va, std::string>>();
  P.Externs = std::make_shared<std::map<Va, Extern>>();
  P.Names = std::make_shared<std::map<Va, std::string>>();
  P.Resolver = std::make_shared<NoNames>();
  return P;
}

} // namespace revenant::lifttest

#endif // REVENANT_UNITTESTS_LIFT_LIFTTESTUTIL_H
