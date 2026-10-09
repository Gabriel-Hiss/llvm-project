//===- unittests/Lift/LiftSemanticsCase.h - differential test cases --------===//
//
// A case is a few x86 instructions plus chosen register, stack and vector
// inputs and the set of outputs the test compares. See LiftSemanticsTest.cpp.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_UNITTESTS_LIFT_LIFTSEMANTICSCASE_H
#define REVENANT_UNITTESTS_LIFT_LIFTSEMANTICSCASE_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace revenant::lifttest {

enum Flag : unsigned { CF = 1, ZF = 2, SF = 4, OF = 8, PF = 16, AllFlags = 31 };

constexpr const char *RegName[10] = {"rax", "rcx", "rdx", "rbx", "rsi",
                                     "rdi", "r8",  "r9",  "r10", "r11"};
// Output buffer layout (u64 slots).
constexpr unsigned SlotFlags = 10;
constexpr unsigned SlotStack = 15;
constexpr unsigned StackQwords = 8;
constexpr unsigned SlotVec = 23;
constexpr unsigned VecRegs = 8;
constexpr unsigned VecQwords = 4;
constexpr unsigned BufferQwords = SlotVec + VecRegs * VecQwords;

struct Case {
  std::string Name;
  std::string Asm;
  std::vector<std::pair<std::string, uint64_t>> Regs;
  std::vector<uint64_t> Stack; // [rsp+0x20 + 8*i]
  std::vector<std::array<uint64_t, 4>> Vec; // xmm/ymm 0..N-1
  std::array<bool, 8> Given{};
  bool VecUsed = false;
  bool Ymm = false;
  unsigned RegMask = 0x3FF;
  unsigned FlagMask = 0;

  Case(std::string N, std::string A) : Name(std::move(N)), Asm(std::move(A)) {}
  Case &r(const char *Reg, uint64_t V) {
    Regs.push_back({Reg, V});
    return *this;
  }
  Case &stack(std::initializer_list<uint64_t> Q) {
    Stack.assign(Q);
    return *this;
  }
  Case &xmm(unsigned N, uint64_t Lo, uint64_t Hi) {
    if (Vec.size() <= N)
      Vec.resize(N + 1);
    Vec[N] = {Lo, Hi, 0, 0};
    Given[N] = true;
    VecUsed = true;
    return *this;
  }
  Case &ymm(unsigned N, uint64_t A, uint64_t B, uint64_t C, uint64_t D) {
    Ymm = true;
    if (Vec.size() <= N)
      Vec.resize(N + 1);
    Vec[N] = {A, B, C, D};
    Given[N] = true;
    VecUsed = true;
    return *this;
  }
  Case &flags(unsigned M) {
    FlagMask = M;
    return *this;
  }
  /// Leave `Reg` out of the comparison (its value is undefined).
  Case &skip(const char *Reg) {
    for (unsigned K = 0; K < 10; ++K)
      if (Reg == std::string(RegName[K]))
        RegMask &= ~(1u << K);
    return *this;
  }
};


/// All differential cases (LiftSemanticsCases.cpp).
std::vector<Case> cases();

} // namespace revenant::lifttest

#endif // REVENANT_UNITTESTS_LIFT_LIFTSEMANTICSCASE_H
