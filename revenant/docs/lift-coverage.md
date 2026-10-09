# Corpus-weighted opaque lift gaps

The census covers all 274 PE builds in `target/parity`, for 66,461,721 static decoded instructions. The CFG-aware function lifter handles control-flow instructions at block terminators, so the opaque-rate denominator excludes the control-flow opcode families listed below. The remaining non-control instructions total 52,255,347; 1,481,683 hit an opaque fallback (2.84%), and 50,773,664 use generated semantics (97.16%).

Control-flow opcodes excluded from this rate account for 14,206,374 static occurrences. The generated table would dispatch 6,175,840 of those through an opaque case when they occur outside a block terminator; the function-level CFG path handles block-final branches, calls, returns, and traps. Per-opcode counts cannot distinguish block position.

The top-200 list below excludes those control-flow families and is weighted by static occurrences across PEs, not unique opcode names. Duplicate binary filenames in the initial census were re-censused into separate temporary directories. Family labels use mnemonic-prefix heuristics. `other` holds remaining non-control opcodes.

Top-200 family entries: ALU/flags 50, SSE/AVX 136, system 3, other 7. No x87 or string opcode appears in this top 200; both can still occur below rank 200.

| Family | Opcode | Static count |
|---|---|---:|
| other | `MOV64mi32` | 532,421 |
| ALU/flags | `AND64mi8` | 74,095 |
| ALU/flags | `AND32mi8` | 61,811 |
| SSE/AVX | `XORPSrr` | 59,467 |
| ALU/flags | `OR32mi8` | 55,366 |
| SSE/AVX | `VMOVDQUYrm` | 50,988 |
| ALU/flags | `ADD8mr` | 24,753 |
| ALU/flags | `ADD32mr` | 23,658 |
| ALU/flags | `ADD64mi8` | 22,505 |
| ALU/flags | `ADD32mi8` | 20,308 |
| SSE/AVX | `VPMOVMSKBYrr` | 18,364 |
| SSE/AVX | `VPSHUFDYri` | 16,773 |
| ALU/flags | `SHL32rCL` | 16,505 |
| SSE/AVX | `VMOVDQUYmr` | 15,137 |
| ALU/flags | `ADD64mr` | 14,757 |
| ALU/flags | `OR32mr` | 14,704 |
| other | `XADD32rm` | 14,592 |
| ALU/flags | `SHR32rCL` | 11,662 |
| SSE/AVX | `VPCMPEQDYrr` | 10,798 |
| SSE/AVX | `VMOVDQAYmr` | 10,199 |
| ALU/flags | `SHL64rCL` | 9,999 |
| SSE/AVX | `VPBLENDVBYrrr` | 8,741 |
| ALU/flags | `SHR64rCL` | 8,481 |
| SSE/AVX | `VMOVNTDQYmr` | 8,054 |
| ALU/flags | `TZCNT32rr` | 7,559 |
| ALU/flags | `OR8mr` | 7,185 |
| ALU/flags | `OR8mi` | 7,124 |
| ALU/flags | `SUB32mi8` | 6,662 |
| SSE/AVX | `VPINSRDrri` | 6,562 |
| SSE/AVX | `VPMASKMOVDYrm` | 6,494 |
| SSE/AVX | `VPSHUFBYrr` | 5,870 |
| SSE/AVX | `VMOVAPSmr` | 5,717 |
| ALU/flags | `BSWAP32r` | 5,633 |
| SSE/AVX | `VMOVDQAYrm` | 5,395 |
| SSE/AVX | `VINSERTF128rri` | 5,309 |
| SSE/AVX | `VPERMQYri` | 5,220 |
| SSE/AVX | `VMOVUPSYmr` | 4,619 |
| SSE/AVX | `VEXTRACTF128rri` | 4,513 |
| SSE/AVX | `VPCMPEQQYrr` | 4,440 |
| ALU/flags | `SUB64mr` | 4,263 |
| SSE/AVX | `ANDPSrr` | 4,151 |
| SSE/AVX | `VMOVAPSrm` | 4,024 |
| ALU/flags | `AND8mi` | 4,012 |
| SSE/AVX | `VMOVDQAmr` | 3,828 |
| SSE/AVX | `VMOVAPSYmr` | 3,705 |
| ALU/flags | `SUB64mi8` | 3,667 |
| SSE/AVX | `VMOVDQUYrr` | 3,659 |
| ALU/flags | `ROR64rCL` | 3,632 |
| SSE/AVX | `VMOVAPSYrm` | 3,535 |
| SSE/AVX | `VMOVUPSYrm` | 3,432 |
| SSE/AVX | `VMOVUPSmr` | 3,378 |
| SSE/AVX | `VPXORYrm` | 3,378 |
| ALU/flags | `LZCNT32rr` | 3,335 |
| ALU/flags | `AND64mr` | 3,310 |
| ALU/flags | `RORX32ri` | 3,305 |
| SSE/AVX | `VPSRLDYri` | 3,224 |
| ALU/flags | `SUB32mr` | 3,143 |
| SSE/AVX | `VMOVDQArm` | 3,110 |
| SSE/AVX | `VMOVPDI2DIrr` | 2,900 |
| SSE/AVX | `VPTESTYrr` | 2,899 |
| SSE/AVX | `MULPSrr` | 2,813 |
| ALU/flags | `OR64mi8` | 2,784 |
| other | `PCMPESTRIrri` | 2,714 |
| SSE/AVX | `VPALIGNRYrri` | 2,686 |
| ALU/flags | `OR32mi` | 2,639 |
| ALU/flags | `ADC32mr` | 2,605 |
| SSE/AVX | `VPMAXSBYrr` | 2,596 |
| SSE/AVX | `VPMINSBYrr` | 2,596 |
| SSE/AVX | `ORPSrr` | 2,576 |
| other | `XADD64rm` | 2,363 |
| SSE/AVX | `AESENCrr` | 2,300 |
| SSE/AVX | `VXORPSrr` | 2,288 |
| SSE/AVX | `VMOVDQUmr` | 2,282 |
| SSE/AVX | `VPSLLVDYrr` | 2,258 |
| SSE/AVX | `VPMINUBYrr` | 2,184 |
| SSE/AVX | `VPMAXSWYrr` | 2,138 |
| SSE/AVX | `VPMINSWYrr` | 2,130 |
| ALU/flags | `ADC8mr` | 2,080 |
| SSE/AVX | `VPCMPGTQYrr` | 2,070 |
| SSE/AVX | `ADDPSrr` | 2,041 |
| SSE/AVX | `VPSLLDYri` | 1,968 |
| ALU/flags | `BSWAP64r` | 1,937 |
| SSE/AVX | `ANDNPSrr` | 1,923 |
| ALU/flags | `AND32mr` | 1,909 |
| SSE/AVX | `PALIGNRrri` | 1,882 |
| SSE/AVX | `COMISDrm` | 1,845 |
| SSE/AVX | `VPSHUFBYrm` | 1,844 |
| SSE/AVX | `VMAXPSYrr` | 1,770 |
| SSE/AVX | `VMINPSYrr` | 1,770 |
| SSE/AVX | `VPMINUWYrr` | 1,770 |
| SSE/AVX | `VFMADD213SDm_Int` | 1,739 |
| SSE/AVX | `VPADDDYrm` | 1,690 |
| SSE/AVX | `VMOVUPDYrm` | 1,665 |
| SSE/AVX | `VMAXPDYrr` | 1,652 |
| SSE/AVX | `VMINPDYrr` | 1,652 |
| SSE/AVX | `VPMAXUBYrr` | 1,594 |
| ALU/flags | `AND64mi32` | 1,575 |
| SSE/AVX | `XORPSrm` | 1,573 |
| SSE/AVX | `VMOVDQUrm` | 1,551 |
| SSE/AVX | `VPERM2I128rri` | 1,548 |
| SSE/AVX | `VMOVMSKPSYrr` | 1,534 |
| SSE/AVX | `VMOVUPSrm` | 1,478 |
| SSE/AVX | `MOVHPSrm` | 1,456 |
| SSE/AVX | `PADDDrm` | 1,452 |
| SSE/AVX | `VPCMPEQBYrr` | 1,432 |
| SSE/AVX | `VSHUFPSYrri` | 1,424 |
| SSE/AVX | `VPERMQYmi` | 1,419 |
| other | `PCMPESTRMrri` | 1,416 |
| SSE/AVX | `VPBROADCASTDYrr` | 1,337 |
| SSE/AVX | `VMOVUPDYmr` | 1,310 |
| SSE/AVX | `VPERMDYrr` | 1,308 |
| SSE/AVX | `CMPPSrri` | 1,302 |
| SSE/AVX | `VPMAXUWYrr` | 1,298 |
| ALU/flags | `SAR32rCL` | 1,252 |
| SSE/AVX | `VPMOVZXBDYrm` | 1,239 |
| ALU/flags | `MULX64rr` | 1,235 |
| ALU/flags | `XOR32mr` | 1,223 |
| ALU/flags | `TZCNT64rr` | 1,166 |
| SSE/AVX | `SUBPSrr` | 1,149 |
| SSE/AVX | `VMOVDQAYrr_REV` | 1,128 |
| SSE/AVX | `VPCMPEQDYrm` | 1,121 |
| SSE/AVX | `MAXPSrr` | 1,110 |
| SSE/AVX | `UCOMISDrm` | 1,097 |
| SSE/AVX | `VPMINUDYrr` | 1,066 |
| SSE/AVX | `VMOVDQU8Z256rm` | 1,064 |
| ALU/flags | `AND32mi` | 1,047 |
| SSE/AVX | `MINPSrr` | 1,046 |
| SSE/AVX | `UCOMISSrm` | 1,033 |
| SSE/AVX | `MOVSDrr` | 1,027 |
| SSE/AVX | `VPCMPGTBYrr` | 1,016 |
| SSE/AVX | `VPCMPEQWYrr` | 1,005 |
| SSE/AVX | `VPCMPEQBYrm` | 982 |
| SSE/AVX | `UNPCKLPSrr` | 971 |
| SSE/AVX | `VPANDNYrr` | 959 |
| SSE/AVX | `CVTPS2PDrr` | 944 |
| SSE/AVX | `VPCMPEQWYrm` | 931 |
| SSE/AVX | `VPCMPEQQYrm` | 885 |
| ALU/flags | `AND16mi8` | 882 |
| SSE/AVX | `VPERMDYrm` | 841 |
| SSE/AVX | `VPERMPDYri` | 832 |
| SSE/AVX | `VMOVUPDYrr` | 826 |
| SSE/AVX | `VSHUFPDYrri` | 826 |
| SSE/AVX | `VPERM2F128rri` | 793 |
| SSE/AVX | `PMINUBrr` | 774 |
| ALU/flags | `XOR64mr` | 774 |
| SSE/AVX | `CVTDQ2PDrr` | 758 |
| ALU/flags | `RORX64ri` | 736 |
| ALU/flags | `OR16mi8` | 719 |
| ALU/flags | `XOR8mr` | 709 |
| SSE/AVX | `PMINUDrr` | 708 |
| SSE/AVX | `VCMPPDYrri` | 708 |
| SSE/AVX | `VCMPPSYrri` | 708 |
| SSE/AVX | `VMOVUPSYrr` | 708 |
| SSE/AVX | `PCLMULQDQrri` | 701 |
| SSE/AVX | `VPBROADCASTQYrr` | 696 |
| SSE/AVX | `VFMADD231SDr_Int` | 694 |
| SSE/AVX | `VMOVDQU8Zrm` | 684 |
| ALU/flags | `SHR8rCL` | 681 |
| SSE/AVX | `VBLENDVPDYrrr` | 669 |
| SSE/AVX | `VPCMPGTDYrr` | 662 |
| SSE/AVX | `VPCMPGTWYrr` | 655 |
| ALU/flags | `AND8mr` | 654 |
| SSE/AVX | `VMASKMOVPDYrm` | 649 |
| SSE/AVX | `VPMAXSDYrr` | 626 |
| SSE/AVX | `VPMINSDYrr` | 618 |
| SSE/AVX | `VCOMISDrm` | 616 |
| SSE/AVX | `VPMADD52HUQZ256m` | 616 |
| SSE/AVX | `VPMADD52LUQZ256m` | 616 |
| SSE/AVX | `KMOVQkm` | 608 |
| SSE/AVX | `VMOVDQU8Z256mr` | 608 |
| SSE/AVX | `VMOVDQU8Zmrk` | 608 |
| SSE/AVX | `VMOVNTDQZmr` | 608 |
| SSE/AVX | `MAXPDrr` | 600 |
| SSE/AVX | `MINPDrr` | 599 |
| SSE/AVX | `VBLENDVPSYrrr` | 598 |
| SSE/AVX | `VPBROADCASTBYrr` | 595 |
| other | `PCMPESTRMrmi` | 590 |
| SSE/AVX | `PMAXSBrr` | 590 |
| SSE/AVX | `PMINSBrr` | 590 |
| SSE/AVX | `VMASKMOVPSYrm` | 590 |
| SSE/AVX | `VPANDNYrm` | 590 |
| ALU/flags | `AND16mr` | 588 |
| ALU/flags | `SHL8rCL` | 588 |
| SSE/AVX | `ANDPSrm` | 571 |
| SSE/AVX | `UNPCKHPDrr` | 543 |
| SSE/AVX | `VPBROADCASTWYrr` | 535 |
| SSE/AVX | `PMINUWrr` | 531 |
| SSE/AVX | `VPMOVZXWDYrm` | 531 |
| SSE/AVX | `CVTDQ2PSrr` | 530 |
| SSE/AVX | `VPINSRQrri` | 529 |
| ALU/flags | `OR16mi` | 509 |
| ALU/flags | `OR64mr` | 509 |
| other | `PACKUSWBrr` | 500 |
| SSE/AVX | `CMPPDrri` | 496 |
| ALU/flags | `OR64mi32` | 490 |
| SSE/AVX | `VPCLMULQDQrri` | 488 |
| SSE/AVX | `PMAXSWrr` | 478 |

| Control-flow opcode | Static count |
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

Distinct opaque opcode names after excluding control flow: 780.
