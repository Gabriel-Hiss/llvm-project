//===- unittests/Lift/LiftSemanticsCases.cpp - differential cases ----------===//
//
// The instruction mix the lifter is checked against native execution on.
//
//===----------------------------------------------------------------------===//

#include "LiftSemanticsCase.h"

#include "llvm/ADT/StringExtras.h"

namespace revenant::lifttest {

static std::string hexQ(uint64_t V) { return "0x" + llvm::utohexstr(V); }

std::vector<Case> cases() {
  std::vector<Case> V;
  auto add = [&](Case C) { V.push_back(std::move(C)); };

  // Read-modify-write memory forms: add/sub/and/or/xor at each width with
  // register and immediate sources.
  const uint64_t Mem = 0x8877665544332211ull;
  for (const char *Op : {"add", "sub", "and", "or", "xor", "adc", "sbb"}) {
    std::string O = Op;
    add(Case(O + " mr64", O + " qword ptr [rsp+0x20], rcx")
            .r("rcx", 0xF0F0F0F0F0F0F0F1ull).stack({Mem}).flags(AllFlags));
    add(Case(O + " mr32", O + " dword ptr [rsp+0x20], ecx")
            .r("rcx", 0xF0F0F0F1ull).stack({Mem}).flags(AllFlags));
    add(Case(O + " mr16", O + " word ptr [rsp+0x20], cx")
            .r("rcx", 0xF0F1ull).stack({Mem}).flags(AllFlags));
    add(Case(O + " mr8", O + " byte ptr [rsp+0x20], cl")
            .r("rcx", 0xF1ull).stack({Mem}).flags(AllFlags));
    add(Case(O + " mi8/64", O + " qword ptr [rsp+0x20], 0x7f")
            .stack({Mem}).flags(AllFlags));
    add(Case(O + " mi8/32 neg", O + " dword ptr [rsp+0x20], -2")
            .stack({Mem}).flags(AllFlags));
    add(Case(O + " mi32/64", O + " qword ptr [rsp+0x20], 0x12345678")
            .stack({Mem}).flags(AllFlags));
    add(Case(O + " mi/8", O + " byte ptr [rsp+0x20], 0x81")
            .stack({Mem}).flags(AllFlags));
    add(Case(O + " rm64", O + " rcx, qword ptr [rsp+0x20]")
            .r("rcx", 0x0123456789ABCDEFull).stack({Mem}).flags(AllFlags));
    add(Case(O + " via pointer", "lea rbx, [rsp+0x20]\n" + O +
                                     " dword ptr [rbx+4], ecx")
            .r("rcx", 0x80000001ull).stack({Mem}).flags(AllFlags).skip("rbx"));
  }
  // Stores.
  add(Case("mov mi32 sx64", "mov qword ptr [rsp+0x20], 0x12345678")
          .stack({Mem}));
  add(Case("mov mi32 sx64 neg", "mov qword ptr [rsp+0x20], -2").stack({Mem}));
  add(Case("mov mi32", "mov dword ptr [rsp+0x24], 0x89abcdef").stack({Mem}));
  add(Case("mov mi16", "mov word ptr [rsp+0x22], 0xabcd").stack({Mem}));
  add(Case("mov mi8", "mov byte ptr [rsp+0x21], 0x99").stack({Mem}));
  add(Case("mov mr8", "mov byte ptr [rsp+0x27], cl")
          .r("rcx", 0xAB).stack({Mem}));

  // inc/dec/neg/not.
  for (const char *Op : {"inc", "dec", "neg", "not"}) {
    std::string O = Op;
    for (uint64_t X : {0ull, 1ull, 0x7fffffffull, 0x80000000ull,
                       0xffffffffull}) {
      add(Case(O + " r32 " + hexQ(X), O + " ecx").r("rcx", X)
              .flags(AllFlags));
      add(Case(O + " m32 " + hexQ(X), O + " dword ptr [rsp+0x20]")
              .stack({X}).flags(AllFlags));
    }
    add(Case(O + " m64", O + " qword ptr [rsp+0x20]")
            .stack({0x8000000000000000ull}).flags(AllFlags));
    add(Case(O + " m8", O + " byte ptr [rsp+0x20]").stack({0x7f})
            .flags(AllFlags));
  }

  // Shifts and rotates: by cl, by imm, by 1; registers and memory. A zero
  // masked count leaves every flag alone; a rotate never touches ZF/SF/PF;
  // OF is defined only for a count of one.
  auto shiftMask = [](const std::string &O, uint64_t N, unsigned W) {
    uint64_t M = N & (W == 64 ? 63 : 31);
    if (M == 0)
      return unsigned(AllFlags);
    bool Rot = O == "rol" || O == "ror";
    unsigned F = ZF | SF | PF;
    if (!Rot && M > W)
      F |= 0;
    else
      F |= CF;
    return F | (M == 1 ? OF : 0u);
  };
  for (const char *Op : {"shl", "shr", "sar", "rol", "ror"}) {
    std::string O = Op;
    for (uint64_t N : {0ull, 1ull, 3ull, 8ull, 31ull, 32ull, 63ull}) {
      add(Case(O + " eax,cl " + std::to_string(N), O + " eax, cl")
              .r("rax", 0x80000001ull).r("rcx", N).flags(shiftMask(O, N, 32)));
      add(Case(O + " rax,cl " + std::to_string(N), O + " rax, cl")
              .r("rax", 0x8000000000000001ull).r("rcx", N)
              .flags(shiftMask(O, N, 64)));
      add(Case(O + " mem32,cl " + std::to_string(N),
               O + " dword ptr [rsp+0x20], cl")
              .stack({0x80000001ull}).r("rcx", N).flags(shiftMask(O, N, 32)));
      add(Case(O + " mem64,cl " + std::to_string(N),
               O + " qword ptr [rsp+0x20], cl")
              .stack({0x8000000000000001ull}).r("rcx", N)
              .flags(shiftMask(O, N, 64)));
    }
    add(Case(O + " eax,1", O + " eax, 1").r("rax", 0xC0000001ull)
            .flags(shiftMask(O, 1, 32)));
    add(Case(O + " mem32,5", O + " dword ptr [rsp+0x20], 5")
            .stack({0xC0000001ull}).flags(shiftMask(O, 5, 32)));
    add(Case(O + " mem8,1", O + " byte ptr [rsp+0x20], 1")
            .stack({0x81ull}).flags(shiftMask(O, 1, 8)));
    add(Case(O + " ax,cl 4", O + " ax, cl").r("rax", 0x8001ull)
            .r("rcx", 4).flags(shiftMask(O, 4, 16)));
    add(Case(O + " al,cl 3", O + " al, cl").r("rax", 0x81ull)
            .r("rcx", 3).flags(shiftMask(O, 3, 8)));
  }

  // XADD.
  add(Case("xadd rr32", "xadd ecx, edx").r("rcx", 0xFFFFFFFFull).r("rdx", 2)
          .flags(AllFlags));
  add(Case("xadd rr64", "xadd rcx, rdx").r("rcx", 5).r("rdx", 7)
          .flags(AllFlags));
  add(Case("xadd m32", "xadd dword ptr [rsp+0x20], ecx").r("rcx", 3)
          .stack({0x7fffffffull}).flags(AllFlags));
  add(Case("xadd m64", "xadd qword ptr [rsp+0x20], rcx").r("rcx", 3)
          .stack({0xffffffffffffffffull}).flags(AllFlags));
  add(Case("lock xadd m32", "lock xadd dword ptr [rsp+0x20], ecx").r("rcx", 9)
          .stack({0x10}).flags(AllFlags));
  add(Case("lock xadd m8", "lock xadd byte ptr [rsp+0x20], cl").r("rcx", 0xF0)
          .stack({0x20}).flags(AllFlags));

  // Bit scans and counts.
  for (uint64_t X : {0ull, 1ull, 0x80ull, 0x8000000000000000ull,
                     0x00f0000000000000ull}) {
    unsigned NZ = X == 0 ? 0 : 1;
    (void)NZ;
    add(Case("tzcnt32 " + hexQ(X), "tzcnt eax, ecx").r("rcx", X)
            .r("rax", 0x55).flags(CF | ZF));
    add(Case("tzcnt64 " + hexQ(X), "tzcnt rax, rcx").r("rcx", X)
            .r("rax", 0x55).flags(CF | ZF));
    add(Case("lzcnt32 " + hexQ(X), "lzcnt eax, ecx").r("rcx", X)
            .r("rax", 0x55).flags(CF | ZF));
    add(Case("lzcnt64 " + hexQ(X), "lzcnt rax, rcx").r("rcx", X)
            .r("rax", 0x55).flags(CF | ZF));
    add(Case("popcnt32 " + hexQ(X), "popcnt eax, ecx").r("rcx", X)
            .r("rax", 0x55).flags(ZF | CF));
    add(Case("popcnt64 " + hexQ(X), "popcnt rax, rcx").r("rcx", X)
            .r("rax", 0x55).flags(ZF | CF));
    Case Bsf("bsf64 " + hexQ(X), "bsf rax, rcx");
    Bsf.r("rcx", X).r("rax", 0x55).flags(ZF);
    if (X == 0) Bsf.skip("rax");
    add(Bsf);
    Case Bsr("bsr32 " + hexQ(X), "bsr eax, ecx");
    Bsr.r("rcx", X).r("rax", 0x55).flags(ZF);
    if ((X & 0xffffffffull) == 0) Bsr.skip("rax");
    add(Bsr);
  }
  add(Case("tzcnt m32", "tzcnt eax, dword ptr [rsp+0x20]").stack({0x100})
          .flags(CF | ZF));
  add(Case("bswap32", "bswap ecx").r("rcx", 0x11223344ull));
  add(Case("bswap64", "bswap rcx").r("rcx", 0x1122334455667788ull));
  add(Case("rorx32", "rorx eax, ecx, 8").r("rcx", 0x11223344ull));
  add(Case("rorx64", "rorx rax, rcx, 12").r("rcx", 0x1122334455667788ull));
  // BMI1/BMI2: bls/andn/bextr/bzhi write only some flags; shiftx/rorx/mulx
  // none; adcx/adox exactly one each.
  for (const char *Op : {"blsr", "blsmsk", "blsi"}) {
    std::string O = Op;
    for (uint64_t X : {0ull, 1ull, 0x10ull, 0x8000000000000000ull,
                       0xffffffffffffffffull}) {
      add(Case(O + " r32 " + hexQ(X), O + " eax, ecx").r("rcx", X)
              .flags(ZF | SF | CF | OF));
      add(Case(O + " r64 " + hexQ(X), O + " rax, rcx").r("rcx", X)
              .flags(ZF | SF | CF | OF));
    }
    add(Case(O + " m64", O + " rax, qword ptr [rsp+0x20]").stack({0x42})
            .flags(ZF | SF | CF | OF));
  }
  // PF is undefined for ANDN per SDM (silicon drives it to 0), so compare
  // the defined four.
  add(Case("andn r32", "andn eax, ecx, edx")
          .r("rcx", 0xFF00FF00ull).r("rdx", 0xFFFF0000ull)
          .flags(ZF | SF | CF | OF));
  add(Case("andn r64", "andn rax, rcx, rdx")
          .r("rcx", 0xFF00FF00FF00FF00ull).r("rdx", 0xFFull)
          .flags(ZF | SF | CF | OF));
  add(Case("andn m64", "andn rax, rcx, qword ptr [rsp+0x20]")
          .r("rcx", 0xFFull).stack({0x0F0F0F0F0F0F0F0Full})
          .flags(ZF | SF | CF | OF));
  // bextr: edx packs [15:8]=len, [7:0]=start. SF/PF/AF are undefined per
  // SDM (silicon drives them to 0); compare only ZF/CF/OF.
  for (uint64_t Ctl : {0x0800ull, 0x0408ull, 0x2000ull, 0x4020ull, 0xff40ull}) {
    add(Case("bextr r64 " + hexQ(Ctl), "bextr rax, rcx, rdx")
            .r("rcx", 0xdeadbeefcafebabeull).r("rdx", Ctl)
            .flags(ZF | CF | OF));
    add(Case("bextr r32 " + hexQ(Ctl), "bextr eax, ecx, edx")
            .r("rcx", 0xdeadbeefull).r("rdx", Ctl).flags(ZF | CF | OF));
  }
  add(Case("bextr m64", "bextr rax, qword ptr [rsp+0x20], rdx")
          .r("rdx", 0x0808ull).stack({0x8877665544332211ull})
          .flags(ZF | CF | OF));
  for (uint64_t Idx : {0ull, 4ull, 32ull, 63ull, 64ull, 200ull}) {
    add(Case("bzhi r64 " + std::to_string(Idx), "bzhi rax, rcx, rdx")
            .r("rcx", 0xffffffffffffffffull).r("rdx", Idx)
            .flags(ZF | SF | CF | OF));
    add(Case("bzhi r32 " + std::to_string(Idx), "bzhi eax, ecx, edx")
            .r("rcx", 0xffffffffull).r("rdx", Idx).flags(ZF | SF | CF | OF));
  }
  for (const char *Op : {"shlx", "shrx", "sarx"}) {
    std::string O = Op;
    for (uint64_t N : {0ull, 1ull, 31ull, 32ull, 63ull, 64ull, 255ull}) {
      add(Case(O + " r32 " + std::to_string(N), O + " eax, ecx, edx")
              .r("rcx", 0x80000001ull).r("rdx", N).flags(AllFlags));
      add(Case(O + " r64 " + std::to_string(N), O + " rax, rcx, rdx")
              .r("rcx", 0x8000000000000001ull).r("rdx", N).flags(AllFlags));
    }
    add(Case(O + " m64", O + " rax, qword ptr [rsp+0x20], rdx")
            .r("rdx", 40).stack({0x8000000000000001ull}).flags(AllFlags));
  }
  add(Case("mulx r64", "mulx rbx, rax, rcx")
          .r("rdx", 0xfffffffffffffffeull).r("rcx", 0xffffffffffffffffull)
          .r("rax", 0x55).r("rbx", 0x77).flags(AllFlags));
  add(Case("mulx r32", "mulx ebx, eax, ecx")
          .r("rdx", 0xfffffffeull).r("rcx", 0x80000001ull)
          .r("rax", 0x55).r("rbx", 0x77).flags(AllFlags));
  add(Case("mulx m64", "mulx rbx, rax, qword ptr [rsp+0x20]")
          .r("rdx", 0x100ull).stack({0x12345678ull}).r("rax", 1)
          .r("rbx", 2).flags(AllFlags));
  add(Case("mulx same dst", "mulx rbx, rbx, rcx")
          .r("rdx", 3).r("rcx", 7).r("rbx", 0x11).flags(AllFlags));
  add(Case("adcx chain", "stc\nadcx eax, ecx\nadcx edx, ecx")
          .r("rax", 0xffffffffull).r("rcx", 1).r("rdx", 0)
          .flags(AllFlags));
  add(Case("adcx m", "stc\nadcx rax, qword ptr [rsp+0x20]")
          .r("rax", 0xffffffffffffffffull).stack({1}).flags(AllFlags));
  add(Case("adox chain", "stc\nadox eax, ecx\nadox edx, ecx")
          .r("rax", 0xffffffffull).r("rcx", 1).r("rdx", 0).flags(AllFlags));
  add(Case("adox m64", "adox rax, qword ptr [rsp+0x20]")
          .r("rax", 0xffffffffffffffffull).stack({1}).flags(AllFlags));

  // Moves and extensions.
  add(Case("movzx r32,m8", "movzx eax, byte ptr [rsp+0x20]").stack({0xF1}));
  add(Case("movsx r64,m8", "movsx rax, byte ptr [rsp+0x20]").stack({0xF1}));
  add(Case("movsx r32,r16", "movsx eax, cx").r("rcx", 0x8001));
  add(Case("movsxd r64,r32", "movsxd rax, ecx").r("rcx", 0x80000001ull));
  add(Case("lea sib", "lea rax, [rcx+rdx*4+0x10]").r("rcx", 100).r("rdx", 7));
  add(Case("lea rsp-rel", "lea rax, [rsp+0x20]\nmov qword ptr [rax], rcx")
          .r("rcx", 0x1234).stack({0}).skip("rax"));
  add(Case("xchg rr", "xchg rax, rcx").r("rax", 1).r("rcx", 2));
  add(Case("cmovl", "cmp eax, ecx\ncmovl edx, ebx").r("rax", 1).r("rcx", 2)
          .r("rbx", 0x77).flags(AllFlags));
  add(Case("setg", "cmp eax, ecx\nsetg dl").r("rax", 3).r("rcx", 2)
          .flags(AllFlags));
  add(Case("push pop", "push rcx\npop rdx").r("rcx", 0xabcdef));
  add(Case("cdq", "cdq").r("rax", 0x80000000ull));
  add(Case("cqo", "cqo").r("rax", 0x8000000000000000ull));

  // Bit tests.
  for (const char *Op : {"bt", "bts", "btr", "btc"}) {
    std::string O = Op;
    add(Case(O + " r32,imm", O + " ecx, 5").r("rcx", 0x20).flags(CF));
    add(Case(O + " r64,r64", O + " rcx, rdx").r("rcx", 0xf0).r("rdx", 68)
            .flags(CF));
    add(Case(O + " m32,imm", O + " dword ptr [rsp+0x20], 33").stack({0x2})
            .flags(CF));
    add(Case(O + " m32,reg big", O + " dword ptr [rsp+0x20], edx")
            .r("rdx", 37).stack({0, 0x20, 0x4}).flags(CF));
    add(Case(O + " m64,reg", O + " qword ptr [rsp+0x28], rdx").r("rdx", 64 + 3)
            .stack({0, 0, 0xff}).flags(CF));
    add(Case(O + " m32,reg neg", O + " dword ptr [rsp+0x28], edx")
            .r("rdx", 0xFFFFFFE1ull).stack({0xfffffffffffffffull, 0xff})
            .flags(CF));
  }

  // Multiply.
  add(Case("imul r,r", "imul eax, ecx").r("rax", 0x10000).r("rcx", 0x10001)
          .flags(CF | OF));
  add(Case("mul r64", "mul rcx").r("rax", 0xffffffffffffull).r("rcx", 0x1000)
          .flags(CF | OF));
  add(Case("div r32", "div ecx").r("rax", 100).r("rdx", 0).r("rcx", 7));
  add(Case("idiv r32", "cdq\nidiv ecx").r("rax", 0xffffff9cull).r("rcx", 7));

  // Vector integer, 128-bit.
  const uint64_t A0 = 0x8070605040302010ull, A1 = 0xFF00FF00FF00FF00ull;
  const uint64_t B0 = 0x8070005040302011ull, B1 = 0xFF01FF00FF00FF00ull;
  for (const char *Op :
       {"pcmpeqb", "pcmpeqw", "pcmpeqd", "pcmpgtb", "pcmpgtw", "pcmpgtd",
        "paddb", "paddw", "paddd", "paddq", "psubb", "psubw", "psubd", "psubq",
        "pand", "por", "pxor", "pandn", "pminub", "pmaxub", "pminsw", "pmaxsw",
        "punpcklbw", "punpcklwd", "punpckldq", "punpcklqdq", "punpckhbw",
        "punpckhwd", "punpckhdq", "punpckhqdq", "pmuludq", "pmullw", "paddusb",
        "paddsw", "psubusb", "psubsw", "pavgb", "packuswb", "packsswb",
        "xorps", "andps", "orps", "andnps"}) {
    add(Case(std::string(Op) + " xmm,xmm", std::string(Op) + " xmm0, xmm1")
            .xmm(0, A0, A1).xmm(1, B0, B1));
    add(Case(std::string(Op) + " xmm,m128", std::string(Op) +
                                              " xmm0, xmmword ptr [rsp+0x20]")
            .xmm(0, A0, A1).stack({B0, B1}));
  }
  // Lane-wise coverage for the remaining packed families (SSE4.1 integer
  // mul/min/max and the qword compare gcc -O3 emits in add_vec-style
  // loops).
  for (const char *Op : {"pmulld", "pminsd", "pminsb", "pmaxsd", "pmaxud",
                         "pmaxuw", "pcmpgtq", "pcmpeqq"}) {
    add(Case(std::string(Op) + " xmm,xmm", std::string(Op) + " xmm0, xmm1")
            .xmm(0, A0, A1).xmm(1, B0, B1));
    add(Case(std::string(Op) + " xmm,m128", std::string(Op) +
                                              " xmm0, xmmword ptr [rsp+0x20]")
            .xmm(0, A0, A1).stack({B0, B1}));
  }
  for (const char *Op : {"phaddd", "phaddw"}) {
    add(Case(std::string(Op) + " xmm,xmm", std::string(Op) + " xmm0, xmm1")
            .xmm(0, A0, A1).xmm(1, B0, B1));
  }
  for (const char *Op : {"vphaddd", "vphaddw"}) {
    add(Case(std::string(Op) + " xmm", std::string(Op) + " xmm2, xmm0, xmm1")
            .xmm(0, A0, A1).xmm(1, B0, B1).xmm(2, 1, 2));
    add(Case(std::string(Op) + " ymm", std::string(Op) + " ymm2, ymm0, ymm1")
            .ymm(0, A0, A1, B0, B1).ymm(1, B0, B1, A0, A1).ymm(2, 1, 2, 3, 4));
  }
  for (const char *Op : {"psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq",
                         "psraw", "psrad"}) {
    add(Case(std::string(Op) + " xmm,imm", std::string(Op) + " xmm0, 3")
            .xmm(0, A0, A1));
    add(Case(std::string(Op) + " xmm,xmm", std::string(Op) + " xmm0, xmm1")
            .xmm(0, A0, A1).xmm(1, 5, 0));
  }
  add(Case("pshufd", "pshufd xmm0, xmm1, 0x1b").xmm(1, A0, A1));
  add(Case("pshuflw", "pshuflw xmm0, xmm1, 0x1b").xmm(1, A0, A1));
  add(Case("pshufb", "pshufb xmm0, xmm1").xmm(0, A0, A1)
          .xmm(1, 0x0001020304050680ull, 0x0f0e0d0c0b0a0908ull));
  add(Case("pmovmskb", "pmovmskb eax, xmm0").xmm(0, A0, A1).r("rax", ~0ull));
  add(Case("movdqu store", "movdqu xmmword ptr [rsp+0x20], xmm0")
          .xmm(0, A0, A1).stack({0, 0, 0}));
  add(Case("movdqa load", "movdqa xmm0, xmmword ptr [rsp+0x20]")
          .stack({B0, B1}).xmm(0, 0, 0));
  add(Case("pslldq", "pslldq xmm0, 5").xmm(0, A0, A1));
  add(Case("psrldq", "psrldq xmm0, 5").xmm(0, A0, A1));

  // VEX 256-bit integer.
  for (const char *Op : {"vpcmpeqb", "vpcmpeqd", "vpcmpeqq", "vpaddd", "vpsubq",
                         "vpand", "vpxor", "vpminub", "vpmaxsw", "vpshufb"}) {
    add(Case(std::string(Op) + " ymm", std::string(Op) + " ymm2, ymm0, ymm1")
            .ymm(0, A0, A1, B0, B1).ymm(1, B0, B1, A0, A1).ymm(2, 1, 2, 3, 4));
  }
  add(Case("vpmovmskb ymm", "vpmovmskb eax, ymm0").ymm(0, A0, A1, B0, B1));
  add(Case("vpshufd ymm", "vpshufd ymm1, ymm0, 0x1b").ymm(0, A0, A1, B0, B1)
          .ymm(1, 9, 9, 9, 9));
  add(Case("vmovdqu ymm store", "vmovdqu ymmword ptr [rsp+0x20], ymm0")
          .ymm(0, A0, A1, B0, B1).stack({0, 0, 0, 0}));
  add(Case("vmovdqu ymm load", "vmovdqu ymm0, ymmword ptr [rsp+0x20]")
          .stack({A0, A1, B0, B1}).ymm(0, 1, 2, 3, 4));
  add(Case("vpsrld ymm", "vpsrld ymm1, ymm0, 3").ymm(0, A0, A1, B0, B1)
          .ymm(1, 9, 9, 9, 9));
  add(Case("vpblendvb ymm", "vpblendvb ymm3, ymm0, ymm1, ymm2")
          .ymm(0, A0, A1, B0, B1).ymm(1, B0, B1, A0, A1)
          .ymm(2, 0x80ff00807f00ff80ull, 0x0080ff00ff008000ull, ~0ull, 0)
          .ymm(3, 7, 7, 7, 7));
  add(Case("vextracti128", "vextracti128 xmm1, ymm0, 1")
          .ymm(0, A0, A1, B0, B1).ymm(1, 9, 9, 9, 9));
  add(Case("vinsertf128", "vinsertf128 ymm2, ymm0, xmm1, 1")
          .ymm(0, A0, A1, B0, B1).ymm(1, 5, 6, 7, 8).ymm(2, 9, 9, 9, 9));
  add(Case("vpxor xmm zeroes upper", "vpxor xmm0, xmm0, xmm1")
          .ymm(0, A0, A1, B0, B1).ymm(1, 1, 2, 3, 4));

  // Scalar and packed float.
  add(Case("xorps self", "xorps xmm0, xmm0").xmm(0, A0, A1));
  add(Case("addps", "addps xmm0, xmm1")
          .xmm(0, 0x3f8000003f800000ull, 0x4000000040000000ull)
          .xmm(1, 0x4040000040400000ull, 0x3f0000003f000000ull));
  add(Case("mulps", "mulps xmm0, xmm1")
          .xmm(0, 0x3f8000003f800000ull, 0x4000000040000000ull)
          .xmm(1, 0x4040000040400000ull, 0x3f0000003f000000ull));
  add(Case("addsd", "addsd xmm0, xmm1")
          .xmm(0, 0x3ff0000000000000ull, 0x1111).xmm(1, 0x4000000000000000ull, 2));
  add(Case("movsd xmm,m64", "movsd xmm0, qword ptr [rsp+0x20]")
          .stack({0x4000000000000000ull}).xmm(0, 0x1111, 0x2222));
  add(Case("movsd m64,xmm", "movsd qword ptr [rsp+0x20], xmm0")
          .stack({0}).xmm(0, 0x4000000000000000ull, 0x2222));
  add(Case("movd xmm,r32", "movd xmm0, ecx").r("rcx", 0x12345678)
          .xmm(0, ~0ull, ~0ull));
  add(Case("movq r64,xmm", "movq rax, xmm0").xmm(0, A0, A1));
  add(Case("movq xmm,r64", "movq xmm0, rcx").r("rcx", A0).xmm(0, ~0ull, ~0ull));
  add(Case("cvtsi2sd", "cvtsi2sd xmm0, ecx").r("rcx", 7).xmm(0, 0, 0));
  add(Case("cvttsd2si", "cvttsd2si eax, xmm0").xmm(0, 0x4014000000000000ull, 0));
  add(Case("comisd", "comisd xmm0, xmm1").xmm(0, 0x3ff0000000000000ull, 0)
          .xmm(1, 0x4000000000000000ull, 0).flags(CF | ZF | PF));
  // Conversion memory forms: the scalar cell loads at its natural width,
  // then the register form's semantics apply. Overflow/NaN produce the
  // x86 indefinite 0x8000..0, which fptosi in the JIT reproduces.
  add(Case("cvtsi2sd m64", "cvtsi2sd xmm0, qword ptr [rsp+0x20]")
          .stack({0xfffffffffffffff0ull}).xmm(0, 0, 0));
  add(Case("cvtsi2ss m32 neg", "cvtsi2ss xmm0, dword ptr [rsp+0x20]")
          .stack({0xaaaaaaaafffffffeull}).xmm(0, 0, 0));
  add(Case("cvttsd2si m64", "cvttsd2si eax, qword ptr [rsp+0x20]")
          .stack({0x4014000000000000ull}));
  add(Case("cvttsd2si m64 inf", "cvttsd2si eax, qword ptr [rsp+0x20]")
          .stack({0x7ff0000000000000ull}));
  add(Case("cvttsd2si m64 nan", "cvttsd2si rax, qword ptr [rsp+0x20]")
          .stack({0x7ff8000000000001ull}));
  add(Case("cvttss2si m32", "cvttss2si ecx, dword ptr [rsp+0x20]")
          .stack({0xbbbbbbbb40a00000ull}));
  add(Case("cvtsd2si64 m64", "cvtsd2si rax, qword ptr [rsp+0x20]")
          .stack({0x4014000000000000ull}));
  add(Case("cvtsi642sd m64", "cvtsi2sd xmm0, qword ptr [rsp+0x20]")
          .stack({0xffffffffffffff9cull}).xmm(0, 0x7777, 0x8888));
  add(Case("vcvtsi2sd m64", "vcvtsi2sd xmm0, xmm1, qword ptr [rsp+0x20]")
          .stack({0xffffffffffffff9cull})
          .xmm(0, 0, 0)
          .xmm(1, 0x1111, 0x2222));
  add(Case("vcvtsi642ss m64", "vcvtsi2ss xmm0, xmm1, qword ptr [rsp+0x20]")
          .stack({0x0000000100000001ull})
          .xmm(0, 0, 0)
          .xmm(1, 0x1111, 0x2222));
  add(Case("vcvttsd2si m64", "vcvttsd2si eax, qword ptr [rsp+0x20]")
          .stack({0x4014000000000000ull}));
  add(Case("vcvttss2si64 m32", "vcvttss2si rax, dword ptr [rsp+0x20]")
          .stack({0xbbbbbbbbbf800000ull}));
  add(Case("vcvtsd2si m64", "vcvtsd2si ecx, qword ptr [rsp+0x20]")
          .stack({0x4014000000000000ull}));
  add(Case("vcvtss2sd m32", "vcvtss2sd xmm0, xmm1, dword ptr [rsp+0x20]")
          .stack({0xcccccccc40a00000ull})
          .xmm(0, 0, 0)
          .xmm(1, 0x1111, 0x2222));
  for (const char *Prefix : {"vfmadd", "vfmsub", "vfnmadd", "vfnmsub"})
    for (const char *Form : {"132sd", "213sd", "231sd"}) {
      std::string Op = std::string(Prefix) + Form;
      add(Case(Op, Op + " xmm0, xmm1, xmm2")
              .xmm(0, 0x4008000000000000ull, 0x1122)
              .xmm(1, 0x4010000000000000ull, 0x2233)
              .xmm(2, 0x4000000000000000ull, 0x3344));
    }
  add(Case("vfmaddsub packed", "vfmaddsub132ps xmm0, xmm1, xmm2")
          .xmm(0, 0x400000003f800000ull, 0x4080000040400000ull)
          .xmm(1, 0x4000000040000000ull, 0x4000000040000000ull)
          .xmm(2, 0x3f8000003f800000ull, 0x3f8000003f800000ull));
  add(Case("vfmsubadd packed", "vfmsubadd231pd xmm0, xmm1, xmm2")
          .xmm(0, 0x4000000000000000ull, 0x4010000000000000ull)
          .xmm(1, 0x3ff0000000000000ull, 0x4000000000000000ull)
          .xmm(2, 0x3ff0000000000000ull, 0x3ff0000000000000ull));
  add(Case("vcvtsd2ss m64", "vcvtsd2ss xmm0, xmm1, qword ptr [rsp+0x20]")
          .stack({0x4014000000000000ull})
          .xmm(0, 0, 0)
          .xmm(1, 0x1111, 0x2222));
  add(Case("aesenc", "aesenc xmm0, xmm1")
          .xmm(0, 0x0011223344556677ull, 0x8899aabbccddeeffull)
          .xmm(1, 0x0f1e2d3c4b5a6978ull, 0x8796a5b4c3d2e1f0ull));
  add(Case("aeskeygenassist", "aeskeygenassist xmm0, xmm1, 0x1b")
          .xmm(1, 0x0011223344556677ull, 0x8899aabbccddeeffull));
  add(Case("pclmulqdq", "pclmulqdq xmm0, xmm1, 0x11")
          .xmm(0, 0x0123456789abcdefull, 0xfedcba9876543210ull)
          .xmm(1, 0x1111222233334444ull, 0xaaaabbbbccccddddull));
  add(Case("crc32 register", "crc32 eax, ecx")
          .r("rax", 0x12345678).r("rcx", 0xdeadbeef).flags(AllFlags));
  add(Case("pcmpistri equal any", "pcmpistri xmm0, xmm1, 0x00")
          .xmm(0, 0x00000000636261ull, 0)
          .xmm(1, 0x00000000636261ull, 0).flags(AllFlags));
  add(Case("pcmpestri equal any", "pcmpestri xmm0, xmm1, 0x00")
          .r("rax", 3).r("rdx", 3)
          .xmm(0, 0x00000000636261ull, 0)
          .xmm(1, 0x00000000636261ull, 0).flags(AllFlags));
  add(Case("pcmpestrm memory", "pcmpestrm xmm0, xmmword ptr [rsp+0x20], 0x00")
          .r("rax", 3).r("rdx", 3)
          .stack({0x00000000636261ull, 0})
          .xmm(0, 0x00000000636261ull, 0).flags(AllFlags));
  add(Case("stmxcsr", "stmxcsr dword ptr [rsp+0x20]")
          .stack({0}).flags(AllFlags));
  add(Case("ldmxcsr", "ldmxcsr dword ptr [rsp+0x20]")
          .stack({0x1f80}).flags(AllFlags));
  add(Case("locked neg m64", "lock neg qword ptr [rsp+0x20]")
          .stack({0x123456789abcdef0ull}).flags(AllFlags));
  add(Case("x87 square root long form",
           "fld qword ptr [rsp+0x20]\n"
           "fsqrt\n"
           "fstp qword ptr [rsp+0x28]")
          .stack({0x4010000000000000ull, 0}));
  add(Case("rdrand non-destination state", "rdrand eax").skip("rax"));
  add(Case("rdseed non-destination state", "rdseed ecx").skip("rcx"));

  // ---- AVX-512 masked moves and ops (k registers) ---------------------------
  // Hosts without AVX-512 skip these (the native half would fault). Merge
  // masking keeps the destination lanes; {z} zeroes the masked-off lanes.
  {
    const uint64_t Q0 = 0x1111111111111111ull, Q1 = 0x2222222222222222ull;
    const uint64_t Q2 = 0x3333333333333333ull, Q3 = 0x4444444444444444ull;
    // k1 = 0x5 (lanes 0 and 2 of a 4-lane qword vector).
    add(Case("avx512 masked vpsubq merge",
             "kmovq k1, rax\n"
             "vpsubq ymm2 {k1}, ymm0, ymm1")
            .r("rax", 0x5).needs("avx512f")
            .zmm(0, Q0, Q1, Q2, Q3, 0, 0, 0, 0)
            .zmm(1, 1, 1, 1, 1, 0, 0, 0, 0)
            .zmm(2, 9, 9, 9, 9, 0, 0, 0, 0));
    add(Case("avx512 masked vpsubq zero",
             "kmovq k1, rax\n"
             "vpsubq ymm2 {k1}{z}, ymm0, ymm1")
            .r("rax", 0xA).needs("avx512f")
            .zmm(0, Q0, Q1, Q2, Q3, 0, 0, 0, 0)
            .zmm(1, 1, 1, 1, 1, 0, 0, 0, 0)
            .zmm(2, 9, 9, 9, 9, 0, 0, 0, 0));
    add(Case("avx512 kmovq k2,k1 roundtrip",
             "kmovq k1, rax\n"
             "kmovq k2, k1\n"
             "kmovq rbx, k2")
            .r("rax", 0xA5A5).r("rbx", 0).needs("avx512f"));
    add(Case("avx512 vpcmpuq into k",
             "kmovq k1, rax\n"
             "vpcmpuq k1, ymm0, ymm1, 0\n"
             "kmovq rbx, k1")
            .r("rax", 0).r("rbx", 0x99).needs("avx512f")
            .zmm(0, 1, 2, 3, 4, 0, 0, 0, 0)
            .zmm(1, 1, 5, 3, 9, 0, 0, 0, 0));
    add(Case("avx512 masked vmovdqu8 store",
             "kmovq k1, rax\n"
             "vmovdqu8 ymmword ptr [rsp+0x20] {k1}, ymm0")
            .r("rax", 0xFF).needs("avx512f")
            .stack({0, 0, 0, 0})
            .zmm(0, Q0, Q1, Q2, Q3, 0, 0, 0, 0));
  }

  // ---- AVX-512 FMA: default rounding and embedded rounding ------------------
  {
    // fma(2.0, 4.0, 3.0) = 11.0 in lane 0 of each form.
    add(Case("avx512 vfmadd213ps default",
             "vfmadd213ps zmm0, zmm1, zmm2")
            .needs("avx512f")
            .zmm(0, 0x4000000040000000ull, 0, 0, 0, 0, 0, 0, 0)
            .zmm(1, 0x4080000040800000ull, 0, 0, 0, 0, 0, 0, 0)
            .zmm(2, 0x4040000040400000ull, 0, 0, 0, 0, 0, 0, 0));
    add(Case("avx512 vfmadd213ps masked merge",
             "kmovq k1, rax\n"
             "vfmadd213ps zmm0 {k1}, zmm1, zmm2")
            .r("rax", 0x1).needs("avx512f")
            .zmm(0, 0x4000000040000000ull, 0x1111111111111111ull, 0, 0, 0, 0, 0, 0)
            .zmm(1, 0x4080000040800000ull, 0x2222222222222222ull, 0, 0, 0, 0, 0, 0)
            .zmm(2, 0x4040000040400000ull, 0x3333333333333333ull, 0, 0, 0, 0, 0, 0));
  }

  // ---- x87 environment, save/restore and BCD --------------------------------
  // The 28-byte environment holds the control word at offset 0 and the status
  // word at offset 4; both round-trip through the modeled state. The images
  // are placed past the eight compared stack qwords (rsp+0x20..0x5f) and the
  // result is read back into a register, so the comparison covers the modeled
  // words only.
  add(Case("x87 fnstenv control word",
           "xor eax, eax\n"
           "fldcw word ptr [rsp+0x20]\n"
           "fnstenv [rsp+0x28]\n"
           "mov ax, word ptr [rsp+0x28]")
          .stack({0x027f}));
  add(Case("x87 fldenv control word",
           "fninit\n"
           "mov word ptr [rsp+0x28], 0x037f\n"
           "fldenv [rsp+0x28]\n"
           "xor eax, eax\n"
           "fnstcw word ptr [rsp+0x40]\n"
           "mov ax, word ptr [rsp+0x40]")
          .stack({0, 0x037f}));
  add(Case("x87 fsave/frstor stack round trip",
           "fldpi\n"
           "fld1\n"
           "fsave [rsp+0x80]\n"
           "frstor [rsp+0x80]\n"
           "fstp qword ptr [rsp+0x40]\n"
           "fstp qword ptr [rsp+0x48]")
          .stack({0, 0, 0, 0, 0, 0}));
  add(Case("x87 fbld packed bcd",
           "fbld [rsp+0x20]\n"
           "fstp qword ptr [rsp+0x40]")
          // 1234: digits 4,3,2,1 in nibbles, sign byte 0.
          .stack({0x0000000000004321ull, 0, 0, 0}));
  add(Case("x87 fbstp packed bcd",
           "fld qword ptr [rsp+0x20]\n"
           "fbstp [rsp+0x60]\n"
           "mov rax, qword ptr [rsp+0x60]")
          .stack({0x4059000000000000ull}));
  add(Case("x87 fxsave/fxrstor control word",
           "fninit\n"
           "fldcw word ptr [rsp+0x20]\n"
           "fxsave [rsp+0x80]\n"
           "fxrstor [rsp+0x80]\n"
           "xor eax, eax\n"
           "fnstcw word ptr [rsp+0x40]\n"
           "mov ax, word ptr [rsp+0x40]")
          .stack({0x027f}));
  // Packed float/integer conversions, including the SSE2 integer-to-double
  // form exercised by compiler-generated vectorized initialization.
  add(Case("sse cvtdq2pd reg", "cvtdq2pd xmm0, xmm1")
          .xmm(1, 0xffffffff00000002ULL, 0)
          .xmm(0, 0, 0));
  add(Case("sse cvtdq2pd mem", "cvtdq2pd xmm0, qword ptr [rsp+0x20]")
          .stack({0xffffffff00000002ULL})
          .xmm(0, 0, 0));
  add(Case("sse cvtdq2pd packed store chain",
           "cvtdq2pd xmm0, xmm1\n"
           "addpd xmm0, xmm2\n"
           "paddd xmm1, xmm3\n"
           "movups xmmword ptr [rsp+0x30], xmm0\n"
           "movups xmmword ptr [rsp+0x40], xmm1")
          .xmm(1, 0xffffffff00000002ULL, 0)
          .xmm(2, 0x3ff0000000000000ULL, 0x4000000000000000ULL)
          .xmm(3, 0x0000000100000001ULL, 0x0000000100000001ULL)
          .stack({0, 0, 0, 0, 0, 0}));
  add(Case("sse cvtdq2ps reg", "cvtdq2ps xmm0, xmm1")
          .xmm(1, 0xffffffff00000002ULL, 0x7fffffff00000001ULL)
          .xmm(0, 0, 0));
  add(Case("sse cvtdq2ps mem", "cvtdq2ps xmm0, xmmword ptr [rsp+0x20]")
          .stack({0xffffffff00000002ULL, 0x7fffffff00000001ULL})
          .xmm(0, 0, 0));
  for (const char *Op : {"cvtps2dq", "cvttps2dq"}) {
    add(Case(std::string("sse ") + Op + " reg",
             std::string(Op) + " xmm0, xmm1")
            .xmm(1, 0xbfc000003fc00000ULL, 0xc098000040500000ULL)
            .xmm(0, 0, 0));
    add(Case(std::string("sse ") + Op + " mem",
             std::string(Op) + " xmm0, xmmword ptr [rsp+0x20]")
            .stack({0xbfc000003fc00000ULL, 0xc098000040500000ULL})
            .xmm(0, 0, 0));
  }
  struct InvalidFP {
    const char *Name;
    uint32_t SingleBits;
    uint64_t DoubleBits;
  };
  for (const InvalidFP &V :
       {InvalidFP{"nan", 0x7fc00000U, 0x7ff8000000000000ULL},
        InvalidFP{"infinity", 0x7f800000U, 0x7ff0000000000000ULL},
        InvalidFP{"out of range", 0x7149f2caU, 0x46293e5939a08ceaULL}}) {
    std::string SetSingle =
        "mov eax, " + hexQ(V.SingleBits) + "\nmovd xmm1, eax\n";
    for (const char *Op : {"cvtps2dq", "cvttps2dq"})
      add(Case(std::string("sse ") + Op + " invalid " + V.Name,
               SetSingle + std::string(Op) + " xmm0, xmm1")
              .xmm(0, 0, 0));
    add(Case(std::string("sse cvttpd2dq invalid ") + V.Name,
             "mov rax, " + hexQ(V.DoubleBits) + "\n"
             "movq xmm1, rax\n"
             "cvttpd2dq xmm0, xmm1")
            .xmm(0, 0, 0));
  }
  add(Case("sse cvttpd2dq reg", "cvttpd2dq xmm0, xmm1")
          .xmm(1, 0x4004000000000000ULL, 0xC014000000000000ULL)
          .xmm(0, 0xaaaaaaaaaaaaaaaaULL, 0xbbbbbbbbbbbbbbbbULL));
  add(Case("sse cvttpd2dq mem", "cvttpd2dq xmm0, xmmword ptr [rsp+0x20]")
          .stack({0x4004000000000000ULL, 0xC014000000000000ULL})
          .xmm(0, 0xaaaaaaaaaaaaaaaaULL, 0xbbbbbbbbbbbbbbbbULL));
  add(Case("sse4.1 cvtps2pd reg", "cvtps2pd xmm0, xmm1")
          .xmm(1, 0x40490FDB3F800000ULL, 0)
          .xmm(0, 0, 0));
  add(Case("sse4.1 cvtps2pd mem",
           "cvtps2pd xmm0, qword ptr [rsp+0x20]")
          .stack({0x40490FDB3F800000ULL})
          .xmm(0, 0, 0));
  add(Case("sse4.1 cvtpd2ps reg", "cvtpd2ps xmm0, xmm1")
          .xmm(1, 0x400921FB54442D18ULL, 0)
          .xmm(0, 0, 0));
  add(Case("sse4.1 cvtpd2ps mem",
           "cvtpd2ps xmm0, xmmword ptr [rsp+0x20]")
          .stack({0x400921FB54442D18ULL, 0})
          .xmm(0, 0, 0));
  for (unsigned Mode = 0; Mode < 4; ++Mode) {
    std::string M = std::to_string(Mode);
    add(Case("sse4.1 roundsd mode " + M,
             "roundsd xmm0, xmm1, " + M)
            .xmm(1, 0x4059200000000000ULL, 0x4053400000000000ULL)
            .xmm(0, 0x3fe0000000000000ULL, 0xbff0000000000000ULL));
    add(Case("sse4.1 roundss mode " + M,
             "roundss xmm0, xmm1, " + M)
            .xmm(1, 0x429a000042c90000ULL, 0)
            .xmm(0, 0xbf8000003f000000ULL, 0xaaaaaaaaaaaaaaaaULL));
  }
  add(Case("sse4.1 roundsd memory mode 1",
           "roundsd xmm0, qword ptr [rsp+0x20], 1")
          .stack({0x4059200000000000ULL})
          .xmm(0, 0x3fe0000000000000ULL, 0xbff0000000000000ULL));
  add(Case("sse4.1 roundss memory mode 2",
           "roundss xmm0, dword ptr [rsp+0x20], 2")
          .stack({0x0000000042c90000ULL})
          .xmm(0, 0xbf8000003f000000ULL, 0xaaaaaaaaaaaaaaaaULL));
  // AVX/AVX2 masked moves suppress masked-off memory accesses and stores.
  add(Case("avx vmaskmovps xmm load",
           "vmaskmovps xmm0, xmm1, xmmword ptr [rsp+0x20]")
          .needs("avx")
          .stack({0x2222222211111111ULL, 0x4444444433333333ULL})
          .xmm(1, 0x0000000080000000ULL, 0x0000000080000000ULL)
          .xmm(0, 0, 0));
  add(Case("avx vmaskmovps xmm store",
           "vmaskmovps xmmword ptr [rsp+0x20], xmm1, xmm2")
          .needs("avx")
          .stack({0xDEADDEADDEADDEADULL, 0xDEADDEADDEADDEADULL})
          .xmm(1, 0x0000000080000000ULL, 0x0000000080000000ULL)
          .xmm(2, 0x2222222211111111ULL, 0x4444444433333333ULL));
  add(Case("avx vmaskmovpd xmm load",
           "vmaskmovpd xmm0, xmm1, xmmword ptr [rsp+0x20]")
          .needs("avx")
          .stack({0x2222222211111111ULL, 0x4444444433333333ULL})
          .xmm(1, 0x8000000000000000ULL, 0)
          .xmm(0, 0, 0));
  add(Case("avx vmaskmovpd xmm store",
           "vmaskmovpd xmmword ptr [rsp+0x20], xmm1, xmm2")
          .needs("avx")
          .stack({0xDEADDEADDEADDEADULL, 0xDEADDEADDEADDEADULL})
          .xmm(1, 0x8000000000000000ULL, 0)
          .xmm(2, 0x2222222211111111ULL, 0x4444444433333333ULL));
  add(Case("avx2 vpmaskmovd xmm load",
           "vpmaskmovd xmm0, xmm1, xmmword ptr [rsp+0x20]")
          .needs("avx2")
          .stack({0x2222222211111111ULL, 0x4444444433333333ULL})
          .xmm(1, 0x0000000080000000ULL, 0x0000000080000000ULL)
          .xmm(0, 0, 0));
  add(Case("avx2 vpmaskmovd xmm store",
           "vpmaskmovd xmmword ptr [rsp+0x20], xmm1, xmm2")
          .needs("avx2")
          .stack({0xDEADDEADDEADDEADULL, 0xDEADDEADDEADDEADULL})
          .xmm(1, 0x0000000080000000ULL, 0x0000000080000000ULL)
          .xmm(2, 0x2222222211111111ULL, 0x4444444433333333ULL));
  add(Case("avx vptest xmm", "vptest xmm0, xmm1")
          .needs("avx")
          .xmm(0, 1, 2)
          .xmm(1, 1, 0)
          .flags(AllFlags));
  add(Case("avx vptest xmm memory", "vptest xmm0, xmmword ptr [rsp+0x20]")
          .needs("avx")
          .xmm(0, 1, 2)
          .stack({1, 0})
          .flags(AllFlags));
  add(Case("avx vmaskmovps ymm load",
           "vmaskmovps ymm0, ymm1, ymmword ptr [rsp+0x20]")
          .needs("avx")
          .stack({0x2222222211111111ULL, 0x4444444433333333ULL,
                  0x6666666655555555ULL, 0x8888888877777777ULL})
          .ymm(1, 0xFFFFFFFF80000000ULL, 0x0000000080000000ULL,
               0xFFFFFFFF80000000ULL, 0x0000000080000000ULL)
          .ymm(0, 0, 0, 0, 0));
  add(Case("avx vmaskmovps ymm store",
           "vmaskmovps ymmword ptr [rsp+0x20], ymm1, ymm2")
          .needs("avx")
          .stack({0xDEADDEADDEADDEADULL, 0xDEADDEADDEADDEADULL,
                  0xDEADDEADDEADDEADULL, 0xDEADDEADDEADDEADULL})
          .ymm(1, 0xFFFFFFFF80000000ULL, 0x0000000080000000ULL,
               0xFFFFFFFF80000000ULL, 0x0000000080000000ULL)
          .ymm(2, 0x2222222211111111ULL, 0x4444444433333333ULL,
               0x6666666655555555ULL, 0x8888888877777777ULL));
  add(Case("avx vmaskmovpd ymm load",
           "vmaskmovpd ymm0, ymm1, ymmword ptr [rsp+0x20]")
          .needs("avx")
          .stack({0x2222222211111111ULL, 0x4444444433333333ULL,
                  0x6666666655555555ULL, 0x8888888877777777ULL})
          .ymm(1, 0x8000000000000000ULL, 0, 0x8000000000000000ULL, 0)
          .ymm(0, 0, 0, 0, 0));
  add(Case("avx vmaskmovpd ymm store",
           "vmaskmovpd ymmword ptr [rsp+0x20], ymm1, ymm2")
          .needs("avx")
          .stack({0xDEADDEADDEADDEADULL, 0xDEADDEADDEADDEADULL,
                  0xDEADDEADDEADDEADULL, 0xDEADDEADDEADDEADULL})
          .ymm(1, 0x8000000000000000ULL, 0, 0x8000000000000000ULL, 0)
          .ymm(2, 0x2222222211111111ULL, 0x4444444433333333ULL,
               0x6666666655555555ULL, 0x8888888877777777ULL));
  add(Case("avx2 vpmaskmovd ymm load",
           "vpmaskmovd ymm0, ymm1, ymmword ptr [rsp+0x20]")
          .needs("avx2")
          .stack({0x2222222211111111ULL, 0x4444444433333333ULL,
                  0x6666666655555555ULL, 0x8888888877777777ULL})
          .ymm(1, 0x0000000080000000ULL, 0x0000000080000000ULL,
               0x0000000080000000ULL, 0x0000000080000000ULL)
          .ymm(0, 0, 0, 0, 0));
  add(Case("avx2 vpmaskmovd ymm store",
           "vpmaskmovd ymmword ptr [rsp+0x20], ymm1, ymm2")
          .needs("avx2")
          .stack({0xDEADDEADDEADDEADULL, 0xDEADDEADDEADDEADULL,
                  0xDEADDEADDEADDEADULL, 0xDEADDEADDEADDEADULL})
          .ymm(1, 0x0000000080000000ULL, 0x0000000080000000ULL,
               0x0000000080000000ULL, 0x0000000080000000ULL)
          .ymm(2, 0x2222222211111111ULL, 0x4444444433333333ULL,
               0x6666666655555555ULL, 0x8888888877777777ULL));
  for (const char *Op : {"vpbroadcastb", "vpbroadcastw", "vpbroadcastd",
                         "vpbroadcastq"}) {
    std::string O = Op;
    unsigned EB = O.back() == 'b' ? 8 : O.back() == 'w' ? 16
                                     : O.back() == 'd'   ? 32
                                                         : 64;
    const char *MemTy = EB == 8  ? "byte ptr"
                        : EB == 16 ? "word ptr"
                        : EB == 32 ? "dword ptr"
                                   : "qword ptr";
    const uint64_t Scalar = 0x8877665544332211ULL;
    for (bool Y : {false, true}) {
      std::string RegName =
          "avx2 " + O + (Y ? " ymm register" : " xmm register");
      Case Reg(RegName, O + (Y ? " ymm0, xmm1" : " xmm0, xmm1"));
      Reg.needs("avx2").xmm(1, Scalar, 0);
      if (Y)
        Reg.ymm(0, 0, 0, 0, 0);
      else
        Reg.xmm(0, 0, 0);
      add(Reg);

      std::string MemName =
          "avx2 " + O + (Y ? " ymm memory" : " xmm memory");
      Case Mem(MemName, O + (Y ? " ymm0, " : " xmm0, ") + MemTy +
                           " [rsp+0x20]");
      Mem.needs("avx2").stack({Scalar});
      if (Y)
        Mem.ymm(0, 0, 0, 0, 0);
      else
        Mem.xmm(0, 0, 0);
      add(Mem);
    }
  }
  add(Case("avx vptest ymm", "vptest ymm0, ymm1")
          .needs("avx")
          .ymm(0, 1, 2, 4, 8)
          .ymm(1, 1, 0, 4, 0)
          .flags(AllFlags));
  return V;
}

} // namespace revenant::lifttest
