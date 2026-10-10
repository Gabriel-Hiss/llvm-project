//===- Avx.td - AVX/AVX2 low-128 lift semantics --------------------------===//
//
// VEX-encoded 128-bit forms the ISel patterns do not cover, plus the two
// low-128 AVX extras the reference models directly (broadcast low byte,
// vcvt low 2xi32 to 2xf64). Ports the VEX paths of crates/lift/src/sse.rs
// and packed.rs.
//
// Same conventions as Sse.td, except every vector write uses rv_setz (VEX
// zeroing) instead of rv_set. Sse.td already covers the VEX forms of every
// op whose legacy form it handles; this file holds the opcodes that exist
// only as VEX (128-bit lanes of AVX/AVX2 ops) plus vzeroupper. Anything
// touching ymm lanes above the low 128 (Y-suffixed defs, in-lane 256-bit
// shuffles, gathers) stays opaque by design: the reference models the low
// 128 only, and the census shows no low-128 consumer needing more.
// YMM-only memory forms (VMOVDQUYrm etc.) are intentionally absent here;
// Swe2LiftFunction's driver keeps them opaque until a consumer needs them.
//
//===----------------------------------------------------------------------===//

// vzeroupper: the reference is a no-op (upper lanes stay unmodeled).
def : RVMatch {
  string Glob = "VZEROUPPER";
  dag Body = (rv_nop);
}

// ---- VEX-128 scalar float arithmetic (dst = src1 with lane 0 replaced) -------
def : RVMatch {
  string Glob = "VADDSSrr_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_ffadd (rv_xlane0f32 (rv_op 0)), (rv_xlane0f32 (rv_op 1)))));
}
def : RVMatch {
  string Glob = "VADDSDrr_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_ffadd (rv_xlane0f64 (rv_op 0)), (rv_xlane0f64 (rv_op 1)))));
}
def : RVMatch {
  string Glob = "VSUBSSrr_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_ffsub (rv_xlane0f32 (rv_op 0)), (rv_xlane0f32 (rv_op 1)))));
}
def : RVMatch {
  string Glob = "VSUBSDrr_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_ffsub (rv_xlane0f64 (rv_op 0)), (rv_xlane0f64 (rv_op 1)))));
}
def : RVMatch {
  string Glob = "VMULSSrr_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_ffmul (rv_xlane0f32 (rv_op 0)), (rv_xlane0f32 (rv_op 1)))));
}
def : RVMatch {
  string Glob = "VMULSDrr_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_ffmul (rv_xlane0f64 (rv_op 0)), (rv_xlane0f64 (rv_op 1)))));
}
def : RVMatch {
  string Glob = "VDIVSSrr_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_ffdiv (rv_xlane0f32 (rv_op 0)), (rv_xlane0f32 (rv_op 1)))));
}
def : RVMatch {
  string Glob = "VDIVSDrr_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_ffdiv (rv_xlane0f64 (rv_op 0)), (rv_xlane0f64 (rv_op 1)))));
}
def : RVMatch {
  string Glob = "VMINSSrr_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_fminnum (rv_xlane0f32 (rv_op 0)), (rv_xlane0f32 (rv_op 1)))));
}
def : RVMatch {
  string Glob = "VMINSDrr_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_fminnum (rv_xlane0f64 (rv_op 0)), (rv_xlane0f64 (rv_op 1)))));
}
def : RVMatch {
  string Glob = "VMAXSSrr_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_fmaxnum (rv_xlane0f32 (rv_op 0)), (rv_xlane0f32 (rv_op 1)))));
}
def : RVMatch {
  string Glob = "VMAXSDrr_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_fmaxnum (rv_xlane0f64 (rv_op 0)), (rv_xlane0f64 (rv_op 1)))));
}
def : RVMatch {
  string Glob = "VSQRTSSr_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_fsqrt (rv_xlane0f32 (rv_op 1)))));
}
def : RVMatch {
  string Glob = "VSQRTSDr_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_fsqrt (rv_xlane0f64 (rv_op 1)))));
}
def : RVMatch {
  string Glob = "VADDSSrm_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_ffadd (rv_xlane0f32 (rv_op 0)), (rv_floadf32 (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "VADDSDrm_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_ffadd (rv_xlane0f64 (rv_op 0)), (rv_floadf64 (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "VSUBSSrm_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_ffsub (rv_xlane0f32 (rv_op 0)), (rv_floadf32 (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "VSUBSDrm_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_ffsub (rv_xlane0f64 (rv_op 0)), (rv_floadf64 (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "VMULSSrm_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_ffmul (rv_xlane0f32 (rv_op 0)), (rv_floadf32 (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "VMULSDrm_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_ffmul (rv_xlane0f64 (rv_op 0)), (rv_floadf64 (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "VDIVSSrm_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_ffdiv (rv_xlane0f32 (rv_op 0)), (rv_floadf32 (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "VDIVSDrm_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_ffdiv (rv_xlane0f64 (rv_op 0)), (rv_floadf64 (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "VSQRTSSm_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_fsqrt (rv_floadf32 (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "VSQRTSDm_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_fsqrt (rv_floadf64 (rv_addr $src2)))));
}

// ---- VEX-128 scalar converts -----------------------------------------------------
def : RVMatch {
  string Glob = "VCVTSI2SSrr_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f32 (rv_op 0), (rv_sitofpf32 (rv_op 1))));
}
def : RVMatch {
  string Glob = "VCVTSI642SSrr_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f32 (rv_op 0), (rv_sitofpf32 (rv_op 1))));
}
def : RVMatch {
  string Glob = "VCVTSI2SDrr_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f64 (rv_op 0), (rv_sitofpf64 (rv_op 1))));
}
def : RVMatch {
  string Glob = "VCVTSI642SDrr_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f64 (rv_op 0), (rv_sitofpf64 (rv_op 1))));
}
def : RVMatch {
  string Glob = "VCVTTSS2SIrr_Int";
  dag Body = (rv_set $dst, (rv_fptosi 32, (rv_xlane0f32 (rv_op 0))));
}
def : RVMatch {
  string Glob = "VCVTTSS2SI64rr_Int";
  dag Body = (rv_set $dst, (rv_fptosi 64, (rv_xlane0f32 (rv_op 0))));
}
def : RVMatch {
  string Glob = "VCVTTSD2SIrr_Int";
  dag Body = (rv_set $dst, (rv_fptosi 32, (rv_xlane0f64 (rv_op 0))));
}
def : RVMatch {
  string Glob = "VCVTTSD2SI64rr_Int";
  dag Body = (rv_set $dst, (rv_fptosi 64, (rv_xlane0f64 (rv_op 0))));
}
def : RVMatch {
  string Glob = "VCVTSS2SDrr_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_fpext (rv_xlane0f32 (rv_op 1)))));
}
def : RVMatch {
  string Glob = "VCVTSD2SSrr_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_fptrunc (rv_xlane0f64 (rv_op 1)))));
}
def : RVMatch {
  string Glob = "VCVTSS2SDrm_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_fpext (rv_floadf32 (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "VCVTSD2SSrm_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_fptrunc (rv_floadf64 (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "VCVTSI2SSrm_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_sitofpf32 (rv_load 32, (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "VCVTSI642SSrm_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_sitofpf32 (rv_load 64, (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "VCVTSI2SDrm_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_sitofpf64 (rv_load 32, (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "VCVTSI642SDrm_Int";
  dag Body = (rv_setz $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_sitofpf64 (rv_load 64, (rv_addr $src2)))));
}
// Memory-source float-to-int: the memory cell is the scalar element width,
// so readOp loads the same bits the register lane carries and the register
// body applies verbatim (0x80..0 indefinite on out-of-range/NaN).
def : RVMatch {
  string Glob = "VCVTTSS2SIrm_Int";
  dag Body = (rv_set $dst, (rv_fptosi 32, (rv_xlane0f32 (rv_op 0))));
}
def : RVMatch {
  string Glob = "VCVTTSS2SI64rm_Int";
  dag Body = (rv_set $dst, (rv_fptosi 64, (rv_xlane0f32 (rv_op 0))));
}
def : RVMatch {
  string Glob = "VCVTTSD2SIrm_Int";
  dag Body = (rv_set $dst, (rv_fptosi 32, (rv_xlane0f64 (rv_op 0))));
}
def : RVMatch {
  string Glob = "VCVTTSD2SI64rm_Int";
  dag Body = (rv_set $dst, (rv_fptosi 64, (rv_xlane0f64 (rv_op 0))));
}
def : RVMatch {
  string Glob = "VCVTSS2SIrm_Int";
  dag Body = (rv_set $dst, (rv_fptosi 32, (rv_xlane0f32 (rv_op 0))));
}
def : RVMatch {
  string Glob = "VCVTSS2SI64rm_Int";
  dag Body = (rv_set $dst, (rv_fptosi 64, (rv_xlane0f32 (rv_op 0))));
}
def : RVMatch {
  string Glob = "VCVTSD2SIrm_Int";
  dag Body = (rv_set $dst, (rv_fptosi 32, (rv_xlane0f64 (rv_op 0))));
}
def : RVMatch {
  string Glob = "VCVTSD2SI64rm_Int";
  dag Body = (rv_set $dst, (rv_fptosi 64, (rv_xlane0f64 (rv_op 0))));
}

// ---- VEX-128 scalar compares (flags only) ------------------------------------------
def : RVMatch {
  string Glob = "VCOMISSrr";
  dag Body = (rv_fcmpf (rv_xlane0f32 (rv_op 0)), (rv_xlane0f32 (rv_op 1)));
}
def : RVMatch {
  string Glob = "VCOMISDrr";
  dag Body = (rv_fcmpf (rv_xlane0f64 (rv_op 0)), (rv_xlane0f64 (rv_op 1)));
}
def : RVMatch {
  string Glob = "VUCOMISSrr";
  dag Body = (rv_fcmpf (rv_xlane0f32 (rv_op 0)), (rv_xlane0f32 (rv_op 1)));
}
def : RVMatch {
  string Glob = "VUCOMISDrr";
  dag Body = (rv_fcmpf (rv_xlane0f64 (rv_op 0)), (rv_xlane0f64 (rv_op 1)));
}
def : RVMatch {
  string Glob = "VCOMISSrm";
  dag Body = (rv_fcmpf (rv_xlane0f32 (rv_op 0)), (rv_floadf32 (rv_addr $src)));
}
def : RVMatch {
  string Glob = "VCOMISDrm";
  dag Body = (rv_fcmpf (rv_xlane0f64 (rv_op 0)), (rv_floadf64 (rv_addr $src)));
}
def : RVMatch {
  string Glob = "VUCOMISSrm";
  dag Body = (rv_fcmpf (rv_xlane0f32 (rv_op 0)), (rv_floadf32 (rv_addr $src)));
}
def : RVMatch {
  string Glob = "VUCOMISDrm";
  dag Body = (rv_fcmpf (rv_xlane0f64 (rv_op 0)), (rv_floadf64 (rv_addr $src)));
}

// ---- VEX-128 packed FP logic (whole 128-bit value) ------------------------------------
def : RVMatch {
  string Glob = "VANDPDrr";
  dag Body = (rv_setz $dst, (rv_and (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "VANDPDrm";
  dag Body = (rv_setz $dst, (rv_and (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "VORPDrr";
  dag Body = (rv_setz $dst, (rv_or (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "VORPDrm";
  dag Body = (rv_setz $dst, (rv_or (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "VXORPDrr";
  dag Body = (rv_setz $dst, (rv_xor (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "VXORPDrm";
  dag Body = (rv_setz $dst, (rv_xor (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "VANDNPDrr";
  dag Body = (rv_setz $dst, (rv_and (rv_not (rv_op 0)), (rv_op 1)));
}
def : RVMatch {
  string Glob = "VANDNPDrm";
  dag Body = (rv_setz $dst, (rv_and (rv_not (rv_op 0)), (rv_op 1)));
}

// ---- VEX-128 packed integer ops ----------------------------------------------------------
// Lane-wise integer arithmetic, bitwise ops and compares: typed pattern
// inversion (X86LiftVector.cpp).
def : RVMatch {
  string Glob = "VPUNPCKLBWr*";
  dag Body = (rv_setz $dst, (rv_punpckl 8, (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "VPUNPCKLWDr*";
  dag Body = (rv_setz $dst, (rv_punpckl 16, (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "VPUNPCKLDQr*";
  dag Body = (rv_setz $dst, (rv_punpckl 32, (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "VPUNPCKLQDQr*";
  dag Body = (rv_setz $dst, (rv_punpckl 64, (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "VPUNPCKHBWr*";
  dag Body = (rv_setz $dst, (rv_punpckh 8, (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "VPUNPCKHWDr*";
  dag Body = (rv_setz $dst, (rv_punpckh 16, (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "VPUNPCKHDQr*";
  dag Body = (rv_setz $dst, (rv_punpckh 32, (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "VPUNPCKHQDQr*";
  dag Body = (rv_setz $dst, (rv_punpckh 64, (rv_op 0), (rv_op 1)));
}

// ---- low-128 AVX extras ----------------------------------------------------------------------
// vbroadcast low byte splat; vcvtdq2pd converts the low 2xi32 to 2xf64.
def : RVMatch {
  string Glob = "VCVTDQ2PDrr";
  dag Body = (rv_setz $dst, (rv_vcvtdq2pd (rv_op 0)));
}
def : RVMatch {
  string Glob = "VCVTDQ2PDrm";
  dag Body = (rv_setz $dst, (rv_vcvtdq2pd (rv_op 0)));
}
def : RVMatch {
  string Glob = "VBROADCASTI128rm";
  dag Body = (rv_setz $dst, (rv_op 0));
}

// ---- full-vector moves (xmm and ymm) ------------------------------------------
// No backend pattern selects these: loads, stores and register copies are
// plain bit copies of the whole vector. VEX forms clear the bits above.
foreach M = ["VMOVDQU", "VMOVDQA", "VMOVAPS", "VMOVAPD", "VMOVUPS", "VMOVUPD"] in {
  foreach W = ["", "Y"] in {
    def : RVMatch { string Glob = M # W # "rm"; dag Body = (rv_setz $dst, (rv_op 0)); }
    def : RVMatch { string Glob = M # W # "mr"; dag Body = (rv_store (rv_addr $dst), (rv_op 1)); }
    def : RVMatch { string Glob = M # W # "rr*"; dag Body = (rv_setz $dst, (rv_op 0)); }
  }
}
foreach M = ["VMOVNTDQ", "VMOVNTPS", "VMOVNTPD"] in
  foreach W = ["", "Y"] in
    def : RVMatch { string Glob = M # W # "mr"; dag Body = (rv_store (rv_addr $dst), (rv_op 1)); }

// AVX-512 EVEX full-vector moves. The unmasked forms carry no ISel pattern
// (NoRMPattern/NoMRPattern) so they are spelled out here; the masked forms
// come from the pattern inverter as a k-register select. Each width is
// enumerated so a glob cannot capture the masked twin and drop the mask.
foreach M = ["VMOVDQU8", "VMOVDQU16", "VMOVDQU32", "VMOVDQU64",
             "VMOVDQA32", "VMOVDQA64", "VMOVAPS", "VMOVAPD",
             "VMOVUPS", "VMOVUPD"] in
  foreach W = ["Z128", "Z256", "Z"] in {
    def : RVMatch {
      string Glob = M # W # "rm";
      dag Body = (rv_setz $dst, (rv_op 0));
    }
    def : RVMatch {
      string Glob = M # W # "mr";
      dag Body = (rv_store (rv_addr $dst), (rv_op 1));
    }
    def : RVMatch {
      string Glob = M # W # "rr";
      dag Body = (rv_setz $dst, (rv_op 0));
    }
  }

// Masked EVEX stores: store only the lanes the k register selects.
foreach M = ["VMOVDQU8", "VMOVDQU16", "VMOVDQU32", "VMOVDQU64",
             "VMOVDQA32", "VMOVDQA64", "VMOVAPS", "VMOVAPD",
             "VMOVUPS", "VMOVUPD"] in
  foreach W = ["Z128", "Z256", "Z"] in
    def : RVMatch {
      string Glob = M # W # "mrk";
      dag Body = (rv_kvstore $dst, $src, $mask);
    }

// FMA3's three encodings permute the tied destination and source operands.
def : RVMatch {
  string Glob = "VFMADD213*";
  dag Body = (rv_setz $dst, (rv_fma "madd", $src1, $src2, $src3));
}
def : RVMatch {
  string Glob = "VFMSUB213*";
  dag Body = (rv_setz $dst, (rv_fma "msub", $src1, $src2, $src3));
}
def : RVMatch {
  string Glob = "VFNMADD213*";
  dag Body = (rv_setz $dst, (rv_fma "nmadd", $src1, $src2, $src3));
}
def : RVMatch {
  string Glob = "VFNMSUB213*";
  dag Body = (rv_setz $dst, (rv_fma "nmsub", $src1, $src2, $src3));
}
def : RVMatch {
  string Glob = "VFMADD132*";
  dag Body = (rv_setz $dst, (rv_fma "madd132", $src1, $src2, $src3));
}
def : RVMatch {
  string Glob = "VFMADD231*";
  dag Body = (rv_setz $dst, (rv_fma "madd231", $src1, $src2, $src3));
}
def : RVMatch {
  string Glob = "VFMSUB132*";
  dag Body = (rv_setz $dst, (rv_fma "msub132", $src1, $src2, $src3));
}
def : RVMatch {
  string Glob = "VFMSUB231*";
  dag Body = (rv_setz $dst, (rv_fma "msub231", $src1, $src2, $src3));
}
def : RVMatch {
  string Glob = "VFNMADD132*";
  dag Body = (rv_setz $dst, (rv_fma "nmadd132", $src1, $src2, $src3));
}
def : RVMatch {
  string Glob = "VFNMADD231*";
  dag Body = (rv_setz $dst, (rv_fma "nmadd231", $src1, $src2, $src3));
}
def : RVMatch {
  string Glob = "VFNMSUB132*";
  dag Body = (rv_setz $dst, (rv_fma "nmsub132", $src1, $src2, $src3));
}
def : RVMatch {
  string Glob = "VFNMSUB231*";
  dag Body = (rv_setz $dst, (rv_fma "nmsub231", $src1, $src2, $src3));
}

foreach Op = ["VFMADDSUB132", "VFMADDSUB213", "VFMADDSUB231",
              "VFMSUBADD132", "VFMSUBADD213", "VFMSUBADD231"] in
  foreach Ty = ["PS", "PD"] in
    foreach Suffix = ["r", "m", "Yr", "Ym"] in
      def : RVMatch {
        string Glob = Op # Ty # Suffix;
        dag Body = (rv_setz $dst,
                    (rv_fmaaddsub Op, $src1, $src2, $src3));
      }

// ---- AVX-512 mask registers and the instructions the pattern inverter
// does not cover ---------------------------------------------------------
//
// The masked vector forms (VMOVDQU*, the arithmetic ops, VFMADD*, ...) come
// from inverted patterns with the k register read as an i64 (see vecExpr).
// What is left is the k-register file itself and the mask-writing compares,
// whose result is a k register rather than a vector.

// KMOV k<->k/m/gpr. $src is the k register, memory or gpr source; the
// destination k register merge-writes the low bits (the emitter decides the
// width from the operand register class).
foreach K = ["KMOVB", "KMOVW", "KMOVD", "KMOVQ"] in {
  def : RVMatch { string Glob = K # "kk*"; dag Body = (rv_kmov $dst, $src); }
  def : RVMatch { string Glob = K # "km*"; dag Body = (rv_kmov $dst, $src); }
  def : RVMatch { string Glob = K # "kr*"; dag Body = (rv_kmov $dst, $src); }
  def : RVMatch { string Glob = K # "mk*"; dag Body = (rv_kst $dst, $src); }
  def : RVMatch { string Glob = K # "rk*"; dag Body = (rv_set $dst, $src); }
}

// VPCMPx: compare two vectors into a k register (predicate = $cc). Operand
// order is ($dst, $src1, $src2, imm); the masked forms AND with $mask.
foreach Suffix = ["B", "W", "D", "Q"] in
  foreach W = ["", "Z", "Z128", "Z256"] in {
    def : RVMatch {
      string Glob = "VPCMP" # Suffix # W # "rr*";
      dag Body = (rv_kcmp "s", $dst, $src1, $src2, $cc);
    }
    def : RVMatch {
      string Glob = "VPCMPU" # Suffix # W # "rr*";
      dag Body = (rv_kcmp "u", $dst, $src1, $src2, $cc);
    }
    def : RVMatch {
      string Glob = "VPCMP" # Suffix # W # "rm*";
      dag Body = (rv_kcmp "s", $dst, $src1, $src2, $cc);
    }
    def : RVMatch {
      string Glob = "VPCMPU" # Suffix # W # "rm*";
      dag Body = (rv_kcmp "u", $dst, $src1, $src2, $cc);
    }
  }
