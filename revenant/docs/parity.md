# Fork parity

The `--export-parity` driver runs fresh PE analysis without loading a PDB and writes the same contract files as the Rust parity exporter: sorted `revenant-items.bin`, a whole-file `file-spans.tsv` partition, and `meta.json`. With `REVENANT_PARITY_ASM=1`, it also writes the optional `revenant-asm.jsonl` sidecar used for the ASM-text metric.

## Full C/C++ corpus

Run from any shell with the parity virtual environment installed. The runner selects every oracle-backed `native` and `gnu` target, enables the ASM sidecar, runs `compare.py`, and writes the per-binary C/C++ table.

```bat
C:\Users\User\RustroverProjects\Revenant\target\parity-venv\Scripts\python.exe C:\Users\User\RustroverProjects\rv-wt\Swe2ParityDriver\revenant\tools\revenant\run_parity.py --revenant-dir C:\Users\User\RustroverProjects\rv-wt\Swe2ParityDriver\build-rv\tools\revenant --parity-root C:\Users\User\RustroverProjects\Revenant\target\parity --tag recovered-ccpp --out C:\Users\User\RustroverProjects\Revenant\target\parity\summary-recovered-ccpp.json
```

Use `--only c-o2` to compare a subset. `--asm-only` recalculates the text metric from sidecars produced by an earlier comparison. The JSON report includes representative records and per-binary counts.

## Rust corpus

Run every oracle-backed Rust target and refresh the separate Rust table:

```bat
C:\Users\User\RustroverProjects\Revenant\target\parity-venv\Scripts\python.exe C:\Users\User\RustroverProjects\rv-wt\Swe2ParityDriver\revenant\tools\revenant\run_parity.py --revenant-dir C:\Users\User\RustroverProjects\rv-wt\Swe2ParityDriver\build-rv\tools\revenant --parity-root C:\Users\User\RustroverProjects\Revenant\target\parity --tag recovered-rust --kinds rust-single rust-cargo --section rust --out C:\Users\User\RustroverProjects\Revenant\target\parity\summary-recovered-rust.json
```

<!-- parity-results:rust-start -->
Rust parity has not been measured yet.
<!-- parity-results:rust-end -->

<!-- parity-results:ccpp-start -->
## Rust corpus

Run all oracle-backed Rust targets, independent of the C/C++ table:

```bat
C:\Users\User\RustroverProjects\Revenant\target\parity-venv\Scripts\python.exe C:\Users\User\RustroverProjects\rv-wt\Swe2ParityDriver\revenant\tools\revenant\run_parity.py --revenant-dir C:\Users\User\RustroverProjects\rv-wt\Swe2ParityDriver\build-rv\tools\revenant --parity-root C:\Users\User\RustroverProjects\Revenant\target\parity --tag recovered-rust --kinds rust-single rust-cargo --section rust --out C:\Users\User\RustroverProjects\Revenant\target\parity\summary-recovered-rust.json
```

<!-- parity-results:rust-start -->
Rust parity has not been measured yet.
<!-- parity-results:rust-end -->


## Metrics

- **Missing / conflicts / extra** are the boundary results from `compare.py`. `extra_on_padding` distinguishes extra instructions that overlap oracle padding runs.
- **ASM text** compares normalized Intel text for matched `(rva, len)` instruction pairs. Register and operand spelling, optional `ptr` sizes, numeric formats, equivalent x86 aliases, and relocation-derived symbol operands are normalized. Near-branch and memory references are compared by their resolved RVA identity. Oracle RIP-relative text may contain an unrelocated object literal, so those targets are decoded from the final PE with iced-x86 before comparison.
- **Clean** requires zero missing, zero conflicts, zero ASM-text mismatches, an `ok` comparison, and a present ASM sidecar.

## Per-binary results

The table is refreshed by `run_parity.py` after each scan; `summary-<tag>.json` contains the full records and owner evidence.

<!-- parity-results:ccpp-start -->
Latest C/C++ scan (`recovered-ccpp`): 325 binaries, 325 clean; missing=0, conflicts=0, ASM mismatches=0.

| Binary | Status | Missing | Conflicts | Extra | ASM checked | ASM mismatch |
|---|---:|---:|---:|---:|---:|---:|
| audio-nodegraph-cl-o1 | ok | 0 | 0 | 25,531 | 177,125 | 0 |
| audio-nodegraph-cl-o2avx2 | ok | 0 | 0 | 29,854 | 178,765 | 0 |
| audio-nodegraph-cl-o2gl | ok | 0 | 0 | 29,956 | 180,648 | 0 |
| audio-nodegraph-cl-ox | ok | 0 | 0 | 29,954 | 179,012 | 0 |
| audio-nodegraph-clang-o1 | ok | 0 | 0 | 25,840 | 175,434 | 0 |
| audio-nodegraph-clang-o2avx2 | ok | 0 | 0 | 26,139 | 176,145 | 0 |
| audio-nodegraph-clang-o2lto | ok | 0 | 0 | 25,807 | 172,462 | 0 |
| audio-nodegraph-clang-o3 | ok | 0 | 0 | 26,023 | 176,453 | 0 |
| audio-nodegraph-clang-oz | ok | 0 | 0 | 25,841 | 175,511 | 0 |
| audio-nodegraph-gcc-o1 | ok | 0 | 0 | 5,236 | 31,195 | 0 |
| audio-nodegraph-gcc-o2hsw | ok | 0 | 0 | 5,239 | 31,167 | 0 |
| audio-nodegraph-gcc-o3 | ok | 0 | 0 | 5,228 | 31,493 | 0 |
| audio-nodegraph-gcc-og | ok | 0 | 0 | 5,658 | 29,620 | 0 |
| audio-nodegraph-gcc-os | ok | 0 | 0 | 5,256 | 28,999 | 0 |
| audio-nodegraph-o2 | ok | 0 | 0 | 29,966 | 178,872 | 0 |
| c-clang-o0 | ok | 0 | 0 | 11,143 | 121,688 | 0 |
| c-clang-o1 | ok | 0 | 0 | 11,020 | 121,200 | 0 |
| c-clang-o2 | ok | 0 | 0 | 11,296 | 121,489 | 0 |
| c-clang-o2avx2 | ok | 0 | 0 | 11,281 | 121,561 | 0 |
| c-clang-o2lto | ok | 0 | 0 | 11,269 | 121,288 | 0 |
| c-clang-o3 | ok | 0 | 0 | 11,041 | 121,527 | 0 |
| c-clang-oz | ok | 0 | 0 | 11,024 | 121,122 | 0 |
| c-gcc-o0 | ok | 0 | 0 | 132 | 3,577 | 0 |
| c-gcc-o1 | ok | 0 | 0 | 132 | 3,126 | 0 |
| c-gcc-o2 | ok | 0 | 0 | 185 | 3,184 | 0 |
| c-gcc-o2hsw | ok | 0 | 0 | 185 | 3,149 | 0 |
| c-gcc-o3 | ok | 0 | 0 | 187 | 3,608 | 0 |
| c-gcc-og | ok | 0 | 0 | 135 | 3,108 | 0 |
| c-gcc-os | ok | 0 | 0 | 149 | 2,982 | 0 |
| c-o0 | ok | 0 | 0 | 10,412 | 120,353 | 0 |
| c-o1 | ok | 0 | 0 | 10,107 | 119,867 | 0 |
| c-o2 | ok | 0 | 0 | 10,357 | 119,975 | 0 |
| c-o2avx2 | ok | 0 | 0 | 10,341 | 119,904 | 0 |
| c-o2gl | ok | 0 | 0 | 10,347 | 119,809 | 0 |
| c-ox | ok | 0 | 0 | 10,186 | 120,361 | 0 |
| cjson-cl-o0 | ok | 0 | 0 | 13,185 | 154,702 | 0 |
| cjson-cl-o1 | ok | 0 | 0 | 11,495 | 152,434 | 0 |
| cjson-cl-o2 | ok | 0 | 0 | 12,228 | 155,608 | 0 |
| cjson-cl-o2avx2 | ok | 0 | 0 | 12,236 | 155,640 | 0 |
| cjson-cl-o2gl | ok | 0 | 0 | 12,217 | 159,452 | 0 |
| cjson-cl-o2pgo | ok | 0 | 0 | 11,944 | 153,507 | 0 |
| cjson-cl-ox | ok | 0 | 0 | 12,456 | 156,334 | 0 |
| cjson-clang-o0 | ok | 0 | 0 | 12,408 | 156,185 | 0 |
| cjson-clang-o1 | ok | 0 | 0 | 12,140 | 153,378 | 0 |
| cjson-clang-o2 | ok | 0 | 0 | 13,002 | 155,466 | 0 |
| cjson-clang-o2avx2 | ok | 0 | 0 | 12,997 | 155,384 | 0 |
| cjson-clang-o2lto | ok | 0 | 0 | 13,026 | 157,017 | 0 |
| cjson-clang-o3 | ok | 0 | 0 | 12,280 | 155,518 | 0 |
| cjson-clang-oz | ok | 0 | 0 | 12,132 | 152,781 | 0 |
| cjson-gcc-o0 | ok | 0 | 0 | 184 | 11,499 | 0 |
| cjson-gcc-o1 | ok | 0 | 0 | 161 | 8,213 | 0 |
| cjson-gcc-o2 | ok | 0 | 0 | 313 | 9,213 | 0 |
| cjson-gcc-o2hsw | ok | 0 | 0 | 300 | 9,149 | 0 |
| cjson-gcc-o3 | ok | 0 | 0 | 304 | 12,162 | 0 |
| cjson-gcc-og | ok | 0 | 0 | 176 | 8,700 | 0 |
| cjson-gcc-os | ok | 0 | 0 | 163 | 7,594 | 0 |
| cpp-cl-cfguard | ok | 0 | 0 | 12,260 | 122,397 | 0 |
| cpp-cl-fp | ok | 0 | 0 | 11,915 | 122,396 | 0 |
| cpp-cl-hotpatch | ok | 0 | 0 | 11,915 | 122,396 | 0 |
| cpp-cl-icf | ok | 0 | 0 | 2,623 | 27,041 | 0 |
| cpp-cl-nogs | ok | 0 | 0 | 11,919 | 122,350 | 0 |
| cpp-clang-cfprot | ok | 0 | 0 | 11,645 | 122,407 | 0 |
| cpp-clang-fp | ok | 0 | 0 | 11,766 | 122,492 | 0 |
| cpp-clang-icf | ok | 0 | 0 | 9,281 | 101,297 | 0 |
| cpp-clang-o0 | ok | 0 | 0 | 12,423 | 124,124 | 0 |
| cpp-clang-o1 | ok | 0 | 0 | 11,395 | 122,363 | 0 |
| cpp-clang-o2 | ok | 0 | 0 | 11,701 | 122,365 | 0 |
| cpp-clang-o2avx2 | ok | 0 | 0 | 11,692 | 122,397 | 0 |
| cpp-clang-o2lto | ok | 0 | 0 | 11,633 | 121,719 | 0 |
| cpp-clang-o3 | ok | 0 | 0 | 11,637 | 122,370 | 0 |
| cpp-clang-oz | ok | 0 | 0 | 11,359 | 122,323 | 0 |
| cpp-gcc-fp | ok | 0 | 0 | 5,355 | 34,306 | 0 |
| cpp-gcc-gc | ok | 0 | 0 | 5,395 | 34,224 | 0 |
| cpp-gcc-o0 | ok | 0 | 0 | 5,862 | 36,245 | 0 |
| cpp-gcc-o1 | ok | 0 | 0 | 5,376 | 34,209 | 0 |
| cpp-gcc-o2 | ok | 0 | 0 | 5,359 | 34,284 | 0 |
| cpp-gcc-o2hsw | ok | 0 | 0 | 5,363 | 34,294 | 0 |
| cpp-gcc-o3 | ok | 0 | 0 | 5,378 | 34,426 | 0 |
| cpp-gcc-og | ok | 0 | 0 | 5,428 | 34,133 | 0 |
| cpp-gcc-os | ok | 0 | 0 | 5,364 | 34,101 | 0 |
| cpp-gcc-pie | ok | 0 | 0 | 5,359 | 34,284 | 0 |
| cpp-gcc-unroll | ok | 0 | 0 | 5,370 | 34,759 | 0 |
| cpp-o0 | ok | 0 | 0 | 11,508 | 122,836 | 0 |
| cpp-o1 | ok | 0 | 0 | 10,517 | 121,662 | 0 |
| cpp-o2 | ok | 0 | 0 | 11,915 | 122,396 | 0 |
| cpp-o2avx2 | ok | 0 | 0 | 11,913 | 122,393 | 0 |
| cpp-o2gl | ok | 0 | 0 | 11,945 | 122,959 | 0 |
| cpp-ox | ok | 0 | 0 | 11,930 | 122,429 | 0 |
| fmt-cl-o0 | ok | 0 | 0 | 50,444 | 319,083 | 0 |
| fmt-cl-o1 | ok | 0 | 0 | 38,829 | 304,358 | 0 |
| fmt-cl-o2 | ok | 0 | 0 | 52,920 | 317,722 | 0 |
| fmt-cl-o2avx2 | ok | 0 | 0 | 52,678 | 317,925 | 0 |
| fmt-cl-o2gl | ok | 0 | 0 | 53,070 | 326,514 | 0 |
| fmt-cl-ox | ok | 0 | 0 | 52,656 | 318,098 | 0 |
| fmt-clang-o0 | ok | 0 | 0 | 49,490 | 316,681 | 0 |
| fmt-clang-o1 | ok | 0 | 0 | 40,011 | 297,478 | 0 |
| fmt-clang-o2 | ok | 0 | 0 | 43,192 | 307,240 | 0 |
| fmt-clang-o2avx2 | ok | 0 | 0 | 43,096 | 307,396 | 0 |
| fmt-clang-o2lto | ok | 0 | 0 | 41,838 | 296,999 | 0 |
| fmt-clang-o3 | ok | 0 | 0 | 42,937 | 307,248 | 0 |
| fmt-clang-oz | ok | 0 | 0 | 39,756 | 294,702 | 0 |
| fmt-gcc-o0 | ok | 0 | 0 | 34,949 | 207,409 | 0 |
| fmt-gcc-o1 | ok | 0 | 0 | 27,083 | 186,038 | 0 |
| fmt-gcc-o2 | ok | 0 | 0 | 26,939 | 184,916 | 0 |
| fmt-gcc-o2hsw | ok | 0 | 0 | 26,887 | 184,897 | 0 |
| fmt-gcc-o3 | ok | 0 | 0 | 26,749 | 192,130 | 0 |
| fmt-gcc-og | ok | 0 | 0 | 29,153 | 179,332 | 0 |
| fmt-gcc-os | ok | 0 | 0 | 27,701 | 176,064 | 0 |
| json-cl-o0 | ok | 0 | 0 | 54,653 | 300,428 | 0 |
| json-cl-o1 | ok | 0 | 0 | 29,901 | 282,057 | 0 |
| json-cl-o2 | ok | 0 | 0 | 59,139 | 305,802 | 0 |
| json-cl-o2avx2 | ok | 0 | 0 | 58,992 | 305,560 | 0 |
| json-cl-o2gl | ok | 0 | 0 | 60,166 | 313,663 | 0 |
| json-cl-ox | ok | 0 | 0 | 58,960 | 305,802 | 0 |
| json-clang-o0 | ok | 0 | 0 | 52,426 | 300,674 | 0 |
| json-clang-o1 | ok | 0 | 0 | 27,559 | 265,944 | 0 |
| json-clang-o2 | ok | 0 | 0 | 32,295 | 287,003 | 0 |
| json-clang-o2avx2 | ok | 0 | 0 | 31,946 | 286,638 | 0 |
| json-clang-o2lto | ok | 0 | 0 | 30,092 | 257,110 | 0 |
| json-clang-o3 | ok | 0 | 0 | 32,295 | 287,003 | 0 |
| json-clang-oz | ok | 0 | 0 | 27,811 | 255,456 | 0 |
| json-gcc-o0 | ok | 0 | 0 | 29,889 | 179,361 | 0 |
| json-gcc-o1 | ok | 0 | 0 | 8,442 | 94,615 | 0 |
| json-gcc-o2 | ok | 0 | 0 | 7,687 | 94,570 | 0 |
| json-gcc-o2hsw | ok | 0 | 0 | 7,703 | 94,092 | 0 |
| json-gcc-o3 | ok | 0 | 0 | 7,224 | 92,477 | 0 |
| json-gcc-og | ok | 0 | 0 | 14,411 | 102,935 | 0 |
| json-gcc-os | ok | 0 | 0 | 8,452 | 78,565 | 0 |
| lua-cl-o0 | ok | 0 | 0 | 25,510 | 217,261 | 0 |
| lua-cl-o1 | ok | 0 | 0 | 13,961 | 206,346 | 0 |
| lua-cl-o2 | ok | 0 | 0 | 20,091 | 219,375 | 0 |
| lua-cl-o2avx2 | ok | 0 | 0 | 20,107 | 219,176 | 0 |
| lua-cl-o2gl | ok | 0 | 0 | 20,031 | 251,063 | 0 |
| lua-cl-ox | ok | 0 | 0 | 23,035 | 222,324 | 0 |
| lua-clang-o0 | ok | 0 | 0 | 15,138 | 220,021 | 0 |
| lua-clang-o1 | ok | 0 | 0 | 13,475 | 202,418 | 0 |
| lua-clang-o2 | ok | 0 | 0 | 18,112 | 215,033 | 0 |
| lua-clang-o2avx2 | ok | 0 | 0 | 18,118 | 214,735 | 0 |
| lua-clang-o2lto | ok | 0 | 0 | 17,100 | 230,316 | 0 |
| lua-clang-o3 | ok | 0 | 0 | 14,298 | 215,143 | 0 |
| lua-clang-oz | ok | 0 | 0 | 13,466 | 199,983 | 0 |
| lua-gcc-o0 | ok | 0 | 0 | 689 | 80,635 | 0 |
| lua-gcc-o1 | ok | 0 | 0 | 618 | 55,472 | 0 |
| lua-gcc-o2 | ok | 0 | 0 | 1,428 | 60,742 | 0 |
| lua-gcc-o2hsw | ok | 0 | 0 | 1,405 | 60,042 | 0 |
| lua-gcc-o3 | ok | 0 | 0 | 1,335 | 75,095 | 0 |
| lua-gcc-og | ok | 0 | 0 | 666 | 57,934 | 0 |
| lua-gcc-os | ok | 0 | 0 | 629 | 52,249 | 0 |
| md4c-cl-o1 | ok | 0 | 0 | 10,130 | 131,709 | 0 |
| md4c-cl-o2avx2 | ok | 0 | 0 | 10,832 | 134,085 | 0 |
| md4c-cl-o2gl | ok | 0 | 0 | 10,773 | 137,663 | 0 |
| md4c-cl-ox | ok | 0 | 0 | 11,329 | 134,557 | 0 |
| md4c-clang-o1 | ok | 0 | 0 | 10,856 | 131,479 | 0 |
| md4c-clang-o2avx2 | ok | 0 | 0 | 11,303 | 135,622 | 0 |
| md4c-clang-o2lto | ok | 0 | 0 | 10,836 | 120,471 | 0 |
| md4c-clang-o3 | ok | 0 | 0 | 10,957 | 135,585 | 0 |
| md4c-clang-oz | ok | 0 | 0 | 10,854 | 130,861 | 0 |
| md4c-gcc-o1 | ok | 0 | 0 | 131 | 13,205 | 0 |
| md4c-gcc-o2hsw | ok | 0 | 0 | 199 | 13,992 | 0 |
| md4c-gcc-o3 | ok | 0 | 0 | 188 | 19,535 | 0 |
| md4c-gcc-og | ok | 0 | 0 | 132 | 14,214 | 0 |
| md4c-gcc-os | ok | 0 | 0 | 123 | 11,517 | 0 |
| md4c-o0 | ok | 0 | 0 | 11,698 | 139,299 | 0 |
| miniz-cl-o1 | ok | 0 | 0 | 10,996 | 153,970 | 0 |
| miniz-cl-o2avx2 | ok | 0 | 0 | 12,182 | 157,033 | 0 |
| miniz-cl-o2gl | ok | 0 | 0 | 12,129 | 158,176 | 0 |
| miniz-cl-ox | ok | 0 | 0 | 12,524 | 157,790 | 0 |
| miniz-clang-o1 | ok | 0 | 0 | 11,792 | 153,936 | 0 |
| miniz-clang-o2avx2 | ok | 0 | 0 | 12,968 | 158,316 | 0 |
| miniz-clang-o2lto | ok | 0 | 0 | 11,948 | 145,828 | 0 |
| miniz-clang-o3 | ok | 0 | 0 | 11,922 | 158,454 | 0 |
| miniz-clang-oz | ok | 0 | 0 | 11,796 | 153,386 | 0 |
| miniz-gcc-o1 | ok | 0 | 0 | 110 | 17,937 | 0 |
| miniz-gcc-o2hsw | ok | 0 | 0 | 282 | 18,267 | 0 |
| miniz-gcc-o3 | ok | 0 | 0 | 266 | 24,300 | 0 |
| miniz-gcc-og | ok | 0 | 0 | 109 | 18,379 | 0 |
| miniz-gcc-os | ok | 0 | 0 | 109 | 15,164 | 0 |
| miniz-o2 | ok | 0 | 0 | 12,190 | 157,133 | 0 |
| pugi-cl-o0 | ok | 0 | 0 | 48,880 | 325,296 | 0 |
| pugi-cl-o1 | ok | 0 | 0 | 38,896 | 309,707 | 0 |
| pugi-cl-o2 | ok | 0 | 0 | 47,333 | 318,069 | 0 |
| pugi-cl-o2avx2 | ok | 0 | 0 | 47,326 | 318,000 | 0 |
| pugi-cl-o2gl | ok | 0 | 0 | 47,331 | 333,754 | 0 |
| pugi-cl-ox | ok | 0 | 0 | 47,026 | 321,858 | 0 |
| pugi-clang-o0 | ok | 0 | 0 | 42,415 | 321,933 | 0 |
| pugi-clang-o1 | ok | 0 | 0 | 40,573 | 305,620 | 0 |
| pugi-clang-o2 | ok | 0 | 0 | 45,028 | 320,156 | 0 |
| pugi-clang-o2avx2 | ok | 0 | 0 | 45,145 | 319,903 | 0 |
| pugi-clang-o2lto | ok | 0 | 0 | 41,779 | 294,781 | 0 |
| pugi-clang-o3 | ok | 0 | 0 | 41,785 | 320,381 | 0 |
| pugi-clang-oz | ok | 0 | 0 | 40,255 | 300,708 | 0 |
| pugi-gcc-o0 | ok | 0 | 0 | 26,639 | 209,597 | 0 |
| pugi-gcc-o1 | ok | 0 | 0 | 26,422 | 197,960 | 0 |
| pugi-gcc-o2 | ok | 0 | 0 | 26,998 | 198,226 | 0 |
| pugi-gcc-o2hsw | ok | 0 | 0 | 26,990 | 197,934 | 0 |
| pugi-gcc-o3 | ok | 0 | 0 | 26,982 | 202,991 | 0 |
| pugi-gcc-og | ok | 0 | 0 | 26,484 | 192,316 | 0 |
| pugi-gcc-os | ok | 0 | 0 | 26,437 | 185,189 | 0 |
| qoi-cl-o1 | ok | 0 | 0 | 9,896 | 119,480 | 0 |
| qoi-cl-o2avx2 | ok | 0 | 0 | 9,915 | 119,481 | 0 |
| qoi-cl-o2gl | ok | 0 | 0 | 9,921 | 119,468 | 0 |
| qoi-cl-ox | ok | 0 | 0 | 9,915 | 119,481 | 0 |
| qoi-clang-o1 | ok | 0 | 0 | 10,752 | 120,787 | 0 |
| qoi-clang-o2avx2 | ok | 0 | 0 | 10,756 | 120,795 | 0 |
| qoi-clang-o2lto | ok | 0 | 0 | 10,751 | 120,274 | 0 |
| qoi-clang-o3 | ok | 0 | 0 | 10,766 | 120,815 | 0 |
| qoi-clang-oz | ok | 0 | 0 | 10,751 | 120,829 | 0 |
| qoi-gcc-o1 | ok | 0 | 0 | 109 | 2,641 | 0 |
| qoi-gcc-o2hsw | ok | 0 | 0 | 109 | 2,635 | 0 |
| qoi-gcc-o3 | ok | 0 | 0 | 116 | 2,869 | 0 |
| qoi-gcc-og | ok | 0 | 0 | 114 | 2,707 | 0 |
| qoi-gcc-os | ok | 0 | 0 | 112 | 2,576 | 0 |
| qoi-o2 | ok | 0 | 0 | 10,777 | 120,815 | 0 |
| spdlog-cl-o0 | ok | 0 | 0 | 82,690 | 412,868 | 0 |
| spdlog-cl-o1 | ok | 0 | 0 | 45,029 | 367,212 | 0 |
| spdlog-cl-o2 | ok | 0 | 0 | 93,108 | 393,148 | 0 |
| spdlog-cl-o2avx2 | ok | 0 | 0 | 92,463 | 393,095 | 0 |
| spdlog-cl-o2gl | ok | 0 | 0 | 94,034 | 401,357 | 0 |
| spdlog-cl-ox | ok | 0 | 0 | 92,084 | 394,316 | 0 |
| spdlog-clang-o0 | ok | 0 | 0 | 75,713 | 407,948 | 0 |
| spdlog-clang-o1 | ok | 0 | 0 | 42,203 | 352,543 | 0 |
| spdlog-clang-o2 | ok | 0 | 0 | 51,808 | 373,548 | 0 |
| spdlog-clang-o2avx2 | ok | 0 | 0 | 51,770 | 374,046 | 0 |
| spdlog-clang-o2lto | ok | 0 | 0 | 47,683 | 346,224 | 0 |
| spdlog-clang-o3 | ok | 0 | 0 | 49,973 | 373,641 | 0 |
| spdlog-clang-oz | ok | 0 | 0 | 42,234 | 345,173 | 0 |
| spdlog-gcc-o0 | ok | 0 | 0 | 68,552 | 348,654 | 0 |
| spdlog-gcc-o1 | ok | 0 | 0 | 36,187 | 244,467 | 0 |
| spdlog-gcc-o2 | ok | 0 | 0 | 35,584 | 246,686 | 0 |
| spdlog-gcc-o2hsw | ok | 0 | 0 | 35,648 | 246,803 | 0 |
| spdlog-gcc-o3 | ok | 0 | 0 | 35,424 | 260,209 | 0 |
| spdlog-gcc-og | ok | 0 | 0 | 42,562 | 237,766 | 0 |
| spdlog-gcc-os | ok | 0 | 0 | 37,127 | 223,472 | 0 |
| sqlite-cl-o0 | ok | 0 | 0 | 42,385 | 356,619 | 0 |
| sqlite-cl-o1 | ok | 0 | 0 | 14,283 | 308,014 | 0 |
| sqlite-cl-o2 | ok | 0 | 0 | 29,755 | 383,320 | 0 |
| sqlite-cl-o2avx2 | ok | 0 | 0 | 29,687 | 383,257 | 0 |
| sqlite-cl-o2gl | ok | 0 | 0 | 29,674 | 412,568 | 0 |
| sqlite-cl-ox | ok | 0 | 0 | 34,872 | 394,724 | 0 |
| sqlite-clang-o0 | ok | 0 | 0 | 15,011 | 355,603 | 0 |
| sqlite-clang-o1 | ok | 0 | 0 | 11,721 | 306,646 | 0 |
| sqlite-clang-o2 | ok | 0 | 0 | 23,784 | 398,765 | 0 |
| sqlite-clang-o2avx2 | ok | 0 | 0 | 23,794 | 402,213 | 0 |
| sqlite-clang-o2lto | ok | 0 | 0 | 25,217 | 390,751 | 0 |
| sqlite-clang-o3 | ok | 0 | 0 | 13,415 | 399,109 | 0 |
| sqlite-clang-oz | ok | 0 | 0 | 11,710 | 283,208 | 0 |
| sqlite-gcc-o0 | ok | 0 | 0 | 145 | 253,809 | 0 |
| sqlite-gcc-o1 | ok | 0 | 0 | 151 | 166,432 | 0 |
| sqlite-gcc-o2 | ok | 0 | 0 | 2,089 | 213,920 | 0 |
| sqlite-gcc-o2hsw | ok | 0 | 0 | 2,109 | 212,807 | 0 |
| sqlite-gcc-o3 | ok | 0 | 0 | 1,926 | 280,990 | 0 |
| sqlite-gcc-og | ok | 0 | 0 | 140 | 170,287 | 0 |
| sqlite-gcc-os | ok | 0 | 0 | 158 | 148,368 | 0 |
| tinyxml2-cl-o0 | ok | 0 | 0 | 15,171 | 157,845 | 0 |
| tinyxml2-cl-o1 | ok | 0 | 0 | 11,919 | 155,529 | 0 |
| tinyxml2-cl-o2 | ok | 0 | 0 | 14,591 | 160,159 | 0 |
| tinyxml2-cl-o2avx2 | ok | 0 | 0 | 14,589 | 160,165 | 0 |
| tinyxml2-cl-o2gl | ok | 0 | 0 | 14,569 | 161,681 | 0 |
| tinyxml2-cl-ox | ok | 0 | 0 | 15,396 | 160,638 | 0 |
| tinyxml2-clang-o0 | ok | 0 | 0 | 14,109 | 159,648 | 0 |
| tinyxml2-clang-o1 | ok | 0 | 0 | 12,522 | 157,731 | 0 |
| tinyxml2-clang-o2 | ok | 0 | 0 | 14,520 | 161,846 | 0 |
| tinyxml2-clang-o2avx2 | ok | 0 | 0 | 14,537 | 161,836 | 0 |
| tinyxml2-clang-o2lto | ok | 0 | 0 | 13,430 | 156,182 | 0 |
| tinyxml2-clang-o3 | ok | 0 | 0 | 13,336 | 161,872 | 0 |
| tinyxml2-clang-oz | ok | 0 | 0 | 12,495 | 156,591 | 0 |
| tinyxml2-gcc-o0 | ok | 0 | 0 | 2,806 | 40,931 | 0 |
| tinyxml2-gcc-o1 | ok | 0 | 0 | 1,994 | 37,092 | 0 |
| tinyxml2-gcc-o2 | ok | 0 | 0 | 2,296 | 40,918 | 0 |
| tinyxml2-gcc-o2hsw | ok | 0 | 0 | 2,314 | 41,009 | 0 |
| tinyxml2-gcc-o3 | ok | 0 | 0 | 2,323 | 47,450 | 0 |
| tinyxml2-gcc-og | ok | 0 | 0 | 2,413 | 36,608 | 0 |
| tinyxml2-gcc-os | ok | 0 | 0 | 2,310 | 35,766 | 0 |
| yaml-cl-o0 | ok | 0 | 0 | 66,591 | 351,545 | 0 |
| yaml-cl-o1 | ok | 0 | 0 | 40,965 | 330,229 | 0 |
| yaml-cl-o2 | ok | 0 | 0 | 71,499 | 354,006 | 0 |
| yaml-cl-o2avx2 | ok | 0 | 0 | 71,672 | 354,424 | 0 |
| yaml-cl-o2gl | ok | 0 | 0 | 72,463 | 369,617 | 0 |
| yaml-cl-ox | ok | 0 | 0 | 70,245 | 355,172 | 0 |
| yaml-clang-o0 | ok | 0 | 0 | 61,919 | 352,941 | 0 |
| yaml-clang-o1 | ok | 0 | 0 | 40,272 | 327,119 | 0 |
| yaml-clang-o2 | ok | 0 | 0 | 47,249 | 340,917 | 0 |
| yaml-clang-o2avx2 | ok | 0 | 0 | 46,999 | 342,176 | 0 |
| yaml-clang-o2lto | ok | 0 | 0 | 44,655 | 334,197 | 0 |
| yaml-clang-o3 | ok | 0 | 0 | 45,006 | 341,085 | 0 |
| yaml-clang-oz | ok | 0 | 0 | 40,059 | 314,711 | 0 |
| yaml-gcc-o0 | ok | 0 | 0 | 49,296 | 279,164 | 0 |
| yaml-gcc-o1 | ok | 0 | 0 | 32,344 | 250,143 | 0 |
| yaml-gcc-o2 | ok | 0 | 0 | 32,806 | 257,938 | 0 |
| yaml-gcc-o2hsw | ok | 0 | 0 | 32,514 | 259,030 | 0 |
| yaml-gcc-o3 | ok | 0 | 0 | 32,489 | 287,543 | 0 |
| yaml-gcc-og | ok | 0 | 0 | 36,908 | 229,637 | 0 |
| yaml-gcc-os | ok | 0 | 0 | 32,888 | 223,614 | 0 |
| zlib-cl-cfguard | ok | 0 | 0 | 11,611 | 136,949 | 0 |
| zlib-cl-fp | ok | 0 | 0 | 11,327 | 136,924 | 0 |
| zlib-cl-hotpatch | ok | 0 | 0 | 11,327 | 136,924 | 0 |
| zlib-cl-icf | ok | 0 | 0 | 2,478 | 31,224 | 0 |
| zlib-cl-nogs | ok | 0 | 0 | 11,333 | 136,928 | 0 |
| zlib-cl-o0 | ok | 0 | 0 | 11,897 | 144,583 | 0 |
| zlib-cl-o1 | ok | 0 | 0 | 10,321 | 134,646 | 0 |
| zlib-cl-o2 | ok | 0 | 0 | 11,327 | 136,924 | 0 |
| zlib-cl-o2avx2 | ok | 0 | 0 | 11,312 | 136,638 | 0 |
| zlib-cl-o2gl | ok | 0 | 0 | 11,310 | 138,991 | 0 |
| zlib-cl-ox | ok | 0 | 0 | 11,445 | 137,531 | 0 |
| zlib-clang-cfprot | ok | 0 | 0 | 12,016 | 140,926 | 0 |
| zlib-clang-fp | ok | 0 | 0 | 11,999 | 141,267 | 0 |
| zlib-clang-icf | ok | 0 | 0 | 9,874 | 119,830 | 0 |
| zlib-clang-o0 | ok | 0 | 0 | 11,411 | 145,150 | 0 |
| zlib-clang-o1 | ok | 0 | 0 | 11,120 | 136,617 | 0 |
| zlib-clang-o2 | ok | 0 | 0 | 12,119 | 140,908 | 0 |
| zlib-clang-o2avx2 | ok | 0 | 0 | 12,102 | 140,742 | 0 |
| zlib-clang-o2lto | ok | 0 | 0 | 11,375 | 134,716 | 0 |
| zlib-clang-o3 | ok | 0 | 0 | 11,333 | 140,948 | 0 |
| zlib-clang-oz | ok | 0 | 0 | 11,108 | 135,985 | 0 |
| zlib-gcc-fp | ok | 0 | 0 | 380 | 16,924 | 0 |
| zlib-gcc-gc | ok | 0 | 0 | 1,060 | 16,553 | 0 |
| zlib-gcc-o0 | ok | 0 | 0 | 250 | 24,534 | 0 |
| zlib-gcc-o1 | ok | 0 | 0 | 290 | 15,768 | 0 |
| zlib-gcc-o2 | ok | 0 | 0 | 391 | 16,601 | 0 |
| zlib-gcc-o2hsw | ok | 0 | 0 | 389 | 16,350 | 0 |
| zlib-gcc-o3 | ok | 0 | 0 | 371 | 21,421 | 0 |
| zlib-gcc-og | ok | 0 | 0 | 259 | 15,686 | 0 |
| zlib-gcc-os | ok | 0 | 0 | 240 | 13,848 | 0 |
| zlib-gcc-pie | ok | 0 | 0 | 391 | 16,601 | 0 |
| zlib-gcc-unroll | ok | 0 | 0 | 375 | 26,872 | 0 |
<!-- parity-results:ccpp-end -->
<!-- parity-results:end -->
