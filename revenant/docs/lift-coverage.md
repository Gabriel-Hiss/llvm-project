# Corpus-weighted opaque lift gaps

The census covers all 274 PE builds listed in `target/parity`. Across those builds, the static instruction counts sum to 66,461,721. Opaque fallback handles 537,456 instructions, or 0.81%. The other 65,924,265 instructions, or 99.19%, hit a generated DSL case or a supported direct-pattern case.

The CFG-aware function lifter handles control-flow instructions at block terminators, so the headline opaque rate excludes the control-flow opcode families listed at the end. The remaining non-control instructions total 52,255,347; 55,402 hit an opaque fallback (0.11%, down from the 2.84% baseline measured before the RMW-memory, shift-by-CL, BMI/BMI2 and XADD forms were lifted), and 52,199,945 use generated semantics (99.89%).

Control-flow opcodes excluded from the headline rate account for 14,206,374 static occurrences. The generated table now lifts direct branches, calls and returns (brOp/brccOp/callToOp/retOp); the remaining 482,054 control occurrences (indirect `JMP64r`/`CALL64r`/`CALL64m`, `TRAP`, `INT` and far returns) still dispatch through an opaque case when they occur outside a block terminator.

The census marks opcodes whose generated case starts with an opaque call. It cannot identify runtime-conditional opaque branches from per-opcode counts alone; the only conditional ones left are `NEG8m`/`NEG16m`/`NEG32m`/`NEG64m` (locked memory NEG), 898 static occurrences included in the opaque total above.

These are weighted static instruction occurrences, not distinct opcodes. Several parity binaries share a filename, so the census tool's original per-filename CSVs overwrote each other. Duplicate-name PEs were re-censused into separate directories and aggregated with the rest.

The table lists the 200 most common non-control opaque opcodes across the corpus. Family labels use mnemonic-prefix rules. Top-200 family entries: SSE/AVX 133, system 39, ALU/flags 10, other 11, x87 7. Distinct non-control opaque opcode names: 302 (down from ~798); occurrences below rank 200 total 204.

| Family | Opcode | Static count |
|---|---|---:|
| SSE/AVX | `VPMASKMOVDYrm` | 6,494 |
| SSE/AVX | `VPTESTYrr` | 2,899 |
| SSE/AVX | `PCMPESTRIrri` | 2,714 |
| SSE/AVX | `AESENCrr` | 2,300 |
| SSE/AVX | `VXORPSrr` | 2,288 |
| system | `CPUID` | 2,089 |
| SSE/AVX | `VFMADD213SDm_Int` | 1,739 |
| system | `PAUSE` | 1,591 |
| SSE/AVX | `PCMPESTRMrri` | 1,416 |
| SSE/AVX | `VMOVDQU8Z256rm` | 1,064 |
| SSE/AVX | `CVTPS2PDrr` | 944 |
| SSE/AVX | `CVTDQ2PDrr` | 758 |
| SSE/AVX | `PCLMULQDQrri` | 701 |
| SSE/AVX | `VFMADD231SDr_Int` | 694 |
| SSE/AVX | `VMOVDQU8Zrm` | 684 |
| SSE/AVX | `VMASKMOVPDYrm` | 649 |
| SSE/AVX | `LDMXCSR` | 638 |
| SSE/AVX | `VPMADD52HUQZ256m` | 616 |
| SSE/AVX | `VPMADD52LUQZ256m` | 616 |
| SSE/AVX | `KMOVQkm` | 608 |
| SSE/AVX | `VMOVDQU8Z256mr` | 608 |
| SSE/AVX | `VMOVDQU8Zmrk` | 608 |
| SSE/AVX | `PCMPESTRMrmi` | 590 |
| SSE/AVX | `VMASKMOVPSYrm` | 590 |
| SSE/AVX | `VPCLMULQDQrri` | 488 |
| SSE/AVX | `VMOVDQU8Zmr` | 456 |
| SSE/AVX | `PEXTRWrri` | 452 |
| SSE/AVX | `CMPSSrri_Int` | 439 |
| SSE/AVX | `VALIGNQZ256rri` | 428 |
| SSE/AVX | `VPBROADCASTDYrm` | 418 |
| SSE/AVX | `VPHADDDYrr` | 368 |
| system | `XGETBV` | 345 |
| system | `INSB` | 325 |
| SSE/AVX | `STMXCSR` | 324 |
| system | `IN8rr` | 317 |
| system | `IN32rr` | 315 |
| SSE/AVX | `VFMADD231SDm_Int` | 297 |
| SSE/AVX | `VMINSDrm_Int` | 297 |
| system | `OUTSL` | 296 |
| SSE/AVX | `VMAXSDrm_Int` | 295 |
| system | `INSL` | 290 |
| SSE/AVX | `KMOVBrk` | 276 |
| SSE/AVX | `VPCMPUQZ256rmi` | 276 |
| system | `IN32ri` | 273 |
| system | `HLT` | 271 |
| system | `CLI` | 271 |
| ALU/flags | `ENTER` | 268 |
| system | `OUTSB` | 267 |
| SSE/AVX | `AESKEYGENASSISTrri` | 263 |
| system | `STI` | 260 |
| system | `OUT32ir` | 260 |
| system | `OUT32rr` | 258 |
| system | `OUT8rr` | 258 |
| SSE/AVX | `PHADDDrr` | 236 |
| SSE/AVX | `VMAXSSrm_Int` | 236 |
| SSE/AVX | `VMINSSrm_Int` | 236 |
| SSE/AVX | `VPMASKMOVDYmr` | 236 |
| system | `VERWm` | 233 |
| SSE/AVX | `VAESENCYrr` | 232 |
| SSE/AVX | `AESENCLASTrr` | 230 |
| SSE/AVX | `VMOVDQU8Zrmk` | 228 |
| SSE/AVX | `VPBROADCASTBrZrr` | 228 |
| SSE/AVX | `PMADDWDrr` | 227 |
| ALU/flags | `MOV16ms` | 227 |
| SSE/AVX | `CVTPD2PSrr` | 226 |
| system | `SLDT16m` | 224 |
| system | `OUT8ir` | 221 |
| SSE/AVX | `AESDECrr` | 214 |
| system | `STRm` | 201 |
| SSE/AVX | `VFNMADD231SDr_Int` | 200 |
| system | `LTRm` | 199 |
| system | `RDSEED32r` | 192 |
| system | `IN8ri` | 191 |
| SSE/AVX | `VFMSUB213SDm_Int` | 190 |
| SSE/AVX | `CMPSDrri_Int` | 188 |
| SSE/AVX | `VPBROADCASTWYrm` | 185 |
| SSE/AVX | `VPINSRBrmi` | 183 |
| SSE/AVX | `VFMSUB213SDr_Int` | 177 |
| SSE/AVX | `VPTESTYrm` | 177 |
| SSE/AVX | `VPCLMULQDQYrri` | 176 |
| ALU/flags | `MOV8o64a` | 174 |
| ALU/flags | `MOV16sm` | 167 |
| SSE/AVX | `SHA256RNDS2rr` | 161 |
| SSE/AVX | `VMOVDQU8Z256mrk` | 152 |
| SSE/AVX | `VMOVDQU8Z256rmk` | 152 |
| ALU/flags | `MOV32ao64` | 152 |
| ALU/flags | `MOV8ao64` | 152 |
| SSE/AVX | `KMOVBkr` | 149 |
| SSE/AVX | `VPBLENDDYrri` | 148 |
| system | `LLDT16m` | 148 |
| SSE/AVX | `SHA1RNDS4rri` | 140 |
| system | `VERRm` | 140 |
| SSE/AVX | `VMOVDQA64Z256rr` | 138 |
| SSE/AVX | `VPSUBQZ256rmk` | 138 |
| SSE/AVX | `SHA1NEXTErr` | 135 |
| SSE/AVX | `VFNMADD213SDm_Int` | 130 |
| SSE/AVX | `CVTTPS2DQrr` | 127 |
| SSE/AVX | `CVTTSD2SIrm_Int` | 127 |
| SSE/AVX | `VFMADD213SDr_Int` | 125 |
| ALU/flags | `MOV32o64a` | 120 |
| SSE/AVX | `VPBROADCASTQYrm` | 119 |
| SSE/AVX | `SHA1MSG1rr` | 112 |
| SSE/AVX | `SHA1MSG2rr` | 112 |
| SSE/AVX | `VCVTSI642SDrm_Int` | 111 |
| SSE/AVX | `VPBROADCASTBYrm` | 105 |
| other | `XCH_F` | 98 |
| SSE/AVX | `PINSRBrri` | 92 |
| system | `INVD` | 86 |
| SSE/AVX | `VPEXTRBrri` | 84 |
| SSE/AVX | `PSADBWrr` | 79 |
| system | `STR32r` | 72 |
| SSE/AVX | `PCMPISTRIrri` | 69 |
| SSE/AVX | `CVTPS2DQrr` | 68 |
| SSE/AVX | `VPSADBWYrr` | 65 |
| SSE/AVX | `VPHADDWYrr` | 62 |
| SSE/AVX | `SHA256MSG1rr` | 61 |
| SSE/AVX | `SHA256MSG2rr` | 61 |
| system | `LTRr` | 61 |
| SSE/AVX | `VROUNDSDri_Int` | 61 |
| SSE/AVX | `PHADDWrr` | 59 |
| other | `RCR64r1` | 57 |
| SSE/AVX | `VCMPSDrri_Int` | 57 |
| SSE/AVX | `VPSLLDQYri` | 56 |
| SSE/AVX | `VPSRLDQYri` | 56 |
| SSE/AVX | `CVTTPD2DQrr` | 54 |
| SSE/AVX | `VFNMADD231SDm_Int` | 50 |
| system | `RDRAND32r` | 48 |
| system | `VERRr` | 47 |
| system | `VERWr` | 47 |
| SSE/AVX | `VPINSRBrri` | 47 |
| SSE/AVX | `PMULHUWrr` | 45 |
| SSE/AVX | `VAESENCLASTYrr` | 40 |
| SSE/AVX | `CMPSDrmi_Int` | 40 |
| system | `SLDT32r` | 40 |
| SSE/AVX | `CMPSSrmi_Int` | 32 |
| other | `RCL32mi` | 30 |
| system | `LLDT16r` | 30 |
| SSE/AVX | `VFMADD132SDm_Int` | 30 |
| other | `RCL8m1` | 30 |
| SSE/AVX | `VFNMADD132SDm_Int` | 30 |
| system | `SGDT64m` | 29 |
| SSE/AVX | `HADDPDrr` | 29 |
| SSE/AVX | `VPMOVZXWDYrr` | 29 |
| SSE/AVX | `VPMOVSXDQYrr` | 28 |
| ALU/flags | `MOV32rs` | 24 |
| SSE/AVX | `VPOPCNTBZrr` | 22 |
| SSE/AVX | `AESDECLASTrr` | 22 |
| SSE/AVX | `VPMADDWDYrr` | 22 |
| ALU/flags | `MOV32sr` | 22 |
| x87 | `FARJMP32m` | 22 |
| SSE/AVX | `VCVTTSD2SIrm_Int` | 21 |
| SSE/AVX | `VCMPSDrmi_Int` | 21 |
| SSE/AVX | `VPBROADCASTDrm` | 21 |
| x87 | `FFREEP` | 20 |
| other | `RCL8mi` | 20 |
| other | `RCL32m1` | 20 |
| SSE/AVX | `VFMSUB231SDr_Int` | 20 |
| SSE/AVX | `VPMOVSXWDYrr` | 19 |
| SSE/AVX | `VALIGNQZ256rmi` | 18 |
| other | `RCR32mCL` | 17 |
| system | `SYSRET` | 17 |
| SSE/AVX | `VPCLMULQDQrmi` | 16 |
| other | `RCL8mCL` | 16 |
| SSE/AVX | `VFMADD132SDr_Int` | 16 |
| system | `CLTS` | 15 |
| x87 | `FSTENVm` | 14 |
| other | `RCL32mCL` | 14 |
| SSE/AVX | `CVTTSD2SI64rm_Int` | 14 |
| SSE/AVX | `ROUNDSDri_Int` | 13 |
| system | `SYSCALL` | 13 |
| SSE/AVX | `VCVTTPD2DQrr` | 13 |
| SSE/AVX | `RCPPSr` | 12 |
| SSE/AVX | `VPSADBWrr` | 12 |
| SSE/AVX | `VPBLENDDrri` | 12 |
| x87 | `FBLDm` | 12 |
| SSE/AVX | `VMOVDDUPrm` | 12 |
| SSE/AVX | `VPBROADCASTBrm` | 12 |
| SSE/AVX | `VPCOMPRESSQZrrk` | 11 |
| SSE/AVX | `AESIMCrr` | 11 |
| x87 | `FARCALL32m` | 11 |
| SSE/AVX | `VANDPSrr` | 11 |
| SSE/AVX | `VBROADCASTSDYrm` | 11 |
| ALU/flags | `ADDSUBPSrr` | 10 |
| system | `RDSSPQ` | 10 |
| SSE/AVX | `VCVTPD2PSrr` | 10 |
| SSE/AVX | `VFMSUB132SDm_Int` | 10 |
| SSE/AVX | `VBROADCASTSSrm` | 9 |
| SSE/AVX | `PMULHUWrm` | 9 |
| SSE/AVX | `VPEXTRWmri` | 8 |
| SSE/AVX | `VPEXTRWrri` | 8 |
| SSE/AVX | `VCVTSI2SDrm_Int` | 8 |
| other | `RCR8mCL` | 8 |
| SSE/AVX | `VBLENDPSYrri` | 8 |
| x87 | `FFREE` | 7 |
| x87 | `FLDENVm` | 7 |
| other | `RCR8mi` | 7 |
| SSE/AVX | `VPBLENDDYrmi` | 7 |
| SSE/AVX | `VXORPSrm` | 6 |
| SSE/AVX | `PBLENDWrri` | 6 |
| system | `LAR64rm` | 6 |

| Excluded control-flow opcode | Static count |
|---|---:|
| `JCC_1` | 4,206,696 |
| `CALL64pcrel32` | 4,007,738 |
| `JMP_1` | 1,729,483 |
| `JCC_4` | 1,487,090 |
| `RET64` | 1,219,827 |
| `JMP_4` | 1,011,697 |
| `CALL64m` | 262,411 |
| `TRAP` | 120,913 |
| `INT3` | 61,088 |
| `CALL64r` | 54,061 |
| `JMP64r` | 27,280 |
| `INT` | 11,273 |
| `JMP64m` | 5,150 |
| `LRET32` | 253 |
| `IRET32` | 246 |
| `LRETI32` | 240 |
| `RETI64` | 203 |
| `JRCXZ` | 194 |
| `LOOPE` | 190 |
| `LOOP` | 179 |
| `LOOPNE` | 137 |
| `LRET64` | 9 |
| `LRETI64` | 7 |
| `IRET64` | 4 |
| `LRET16` | 2 |
| `JMP_2` | 1 |
| `LRETI16` | 1 |
| `RETI16` | 1 |

Distinct opaque opcode names in the full census (control included): 318.
