# Pair48 Assembly Development Log

## 2026-10-01 — ESP-IDF 6.1 / ESP32-S3 baseline

### Integration

- Added `pair48_selftest.c` to the main component sources.
- Called `pair48_library_selftest()` before the Livermore benchmark and the
  alternate memory-bottleneck benchmark, so both configured application entry
  points validate the library at startup.

### Assembler change and reason

- The first on-device run passed through the scalar checks, then raised an
  IllegalInstruction exception in `pair48_add_array_asm` at the branch before
  its `loopnez` loop. The assembler emitted executable alignment padding for
  the loop instruction. With the branch not taken, execution fell through that
  padding and decoded it as instructions.
- Updated the zero-pair control flow in `pair48_add_array_asm`,
  `pair48_mul_array_asm`, and `pair48_saxpy_asm`: nonzero pair counts branch
  directly to the aligned loop entry; a zero pair count jumps past the loop to
  the scalar-tail path. This keeps the loop alignment while preventing normal
  fallthrough through its padding. Arithmetic and 64-bit PIE loads/stores were
  left unchanged.

### Verification

- ESP-IDF 6.1 build completed successfully for the ESP32-S3.
- The first firmware run exposed the alignment fault above. After the fix, the
  on-device self-test reported:

  ```text
  scalar mismatches: 0
  bulk mismatches:   0
  C SAXPY:           61.09 cycles/element
  ASM SAXPY:         27.16 cycles/element
  ASM speedup:       2.249x
  ASM throughput:    8.835 M elements/s/core
  ```

- The firmware continued into and completed all three Livermore span sets.
- The SAXPY numbers above are from this self-test configuration (`N=256`,
  `512` repeats, 240 MHz). Compare them with other benchmark results only when
  their measurement setup is equivalent.

## 2026-10-01 — Four-accumulator dot product and Livermore Kernel 3

### Implementation

- Added `pair48_dot_asm(out, x, y, n)` to the public API and assembly library.
- The assembly processes four independent accumulator chains, then combines
  them in the same order as `pair48_dot()`. It also applies remaining elements
  after that reduction, matching the C routine's tail order.
- Added self-test comparisons against `pair48_dot()` for lengths 0 through 9,
  covering no work, tail-only inputs, and grouped iterations with tails.
- Added a separate Kernel 3 measurement for Pair48 assembly. Existing Pair48 C
  and Accurate48 runs remain unchanged; the output reports assembly/C speedup
  and assembly checksum error against Accurate48.

### Verification and first measurements

- ESP-IDF 6.1 build succeeded; the new `wfr` initialization and dot loop
  assembled and linked for the ESP32-S3.
- On-device self-test reported zero scalar mismatches and zero bulk
  mismatches, including all dot lengths from 0 to 9.
- Initial Kernel 3 measurements (M useful FLOP/s; 240 MHz, timing multiplier 4):

  | Span | Pair48 C | Pair48 ASM | ASM/C | ASM checksum relative error vs Accurate48 |
  |---|---:|---:|---:|---:|
  | 1 | 11.148 | 10.171 | 0.912x | 2.000e-14 |
  | 2 | 11.125 | 9.916 | 0.891x | 1.110e-14 |
  | 3 | 11.046 | 9.206 | 0.834x | 0.000e+00 |

- The four-accumulator assembly version is correct but slower than the current
  optimized C path in this first pass. Treat these values as a baseline for
  instruction scheduling work, not as evidence of an assembly speedup.

### Disassembly review, repeat entry point, and rescheduling

- Compared `pair48_dot()` and `pair48_dot_asm()` in the linked ESP32-S3 ELF
  with `xtensa-esp32s3-elf-objdump -d -S`. In the four-element unrolled body,
  the C implementation has about 87 instructions and uses scalar loads for
  the Pair48 fields. Assembly has about 76 instructions and uses eight
  `ee.ldf.64.ip` loads to fetch four x/y pairs. GCC's loop schedules operations
  from later products while it finishes earlier accumulations, which hides
  dependencies better than the original assembly's mostly element-at-a-time
  MAC macro. These are counts for the inspected loop bodies, not whole-function
  totals.
- Changed `PAIR48_DOT_MAC` to begin `acc_hi + product_hi` immediately after
  calculating the rounded high product. This moves work that depends on the
  accumulator into the interval used for the product residual and low product
  terms, reducing avoidable serialization while preserving the reference
  `pair48_add_inline()` grouping. This was an instruction scheduling change;
  the arithmetic formula and reduction order were retained.
- Added `pair48_dot_repeat_asm(out, x, y, n, passes)`. Kernel 3 invokes the
  repeated entry point once for its full pass count, avoiding a C-to-assembly
  call on every dot pass. `pair48_dot_asm()` remains available for one pass
  and shares the same implementation. The repeated routine resets its four
  accumulators for every pass, as the original benchmark does.
- The four Pair48 accumulators occupy eight FP registers; the implementation
  also needs four input and four scratch registers. That uses the 16 FP
  registers available to this routine, so simply adding more full accumulator
  streams would require a staged schedule or register spills. No extra streams
  were added in this revision.
- Extended the self-test to compare the one-pass and three-pass routines with
  `pair48_dot()` for lengths 0 through 9. The complete on-device self-test
  still reported zero scalar mismatches and zero bulk mismatches.
- ESP-IDF 6.1 build and ESP32-S3 run succeeded. Kernel 3 measurements after
  the scheduling and repeat-call changes (M useful FLOP/s; 240 MHz, timing
  multiplier 4):

  | Span | Pair48 C | Pair48 ASM | ASM/C | ASM checksum relative error vs Accurate48 |
  |---|---:|---:|---:|---:|
  | 1 | 11.148 | 10.864 | 0.975x | 2.000e-14 |
  | 2 | 11.125 | 10.603 | 0.953x | 1.110e-14 |
  | 3 | 11.044 | 9.870 | 0.894x | 0.000e+00 |

- The revised assembly improved over its first measured version on all three
  spans (about 6–7% more throughput), but GCC's C implementation remains
  faster, especially on the short third span. The first version's span 3 C
  rate was 11.046 Mflop/s; the small change to 11.044 in this run reflects
  normal measurement variation. Accuracy remains excellent for these tested
  inputs, but this is not broad validation of Pair48 across all kernels.

### Follow-up schedule analysis

- Rechecked the optimized `pair48_dot()` machine code against the repeated
  assembly loop. Both implement the same explicit Pair48 multiply sequence
  (`mul.s`, `msub.s` residual, and two `madd.s` cross terms) followed by the
  same Pair48 add grouping. GCC does not replace it with ordinary FP32 math.
- In GCC's four-element body, it starts the next element's high product while
  it is still completing the previous element's Pair48 accumulation. The
  current assembly macro completes the product and accumulation for one
  element before beginning the next. Its early `acc_hi + product_hi` move
  overlaps part of the current product calculation, but it does not reproduce
  GCC's across-element schedule.
- GCC also uses scalar loads, while assembly retains paired `ee.ldf.64.ip`
  loads and `loopnez`. The paired loads and zero-overhead loop alone did not
  offset the scheduling advantage in these measurements. The repeated entry
  point already removes the per-pass C/assembly call boundary, so that is not
  the remaining long-span gap.
- No further assembly scheduling edit was made in this review: moving the next
  product across the current accumulation requires carefully staging operands,
  product residuals, and the previous accumulator result in the same 16 FP
  registers. A speculative rewrite risks changing rounding/order or causing
  spills; the existing C path already supplies the faster measured Kernel 3
  implementation. Retain Pair48 C for Kernel 3 unless a concrete register
  schedule can be assembled and verified without spills. The recorded
  three-span comparison remains the decision data; this review did not rerun
  the build or board benchmark because it changed no code.

### Staged two-product experiment and Kernel 3 decision

- Implemented a separate `pair48_dot_staged_repeat_asm` experiment. Each
  unrolled pair retained four independent Pair48 accumulators and used paired
  `ee.ldf.64.ip` loads plus `loopnez`. It delayed product 0's accumulator low
  word while beginning product 1's high product and residual, preserving the
  Pair48 multiply formula, per-accumulator update order, and final reduction.
- ESP-IDF 6.1 build succeeded. The disassembled hot loop contains no temporary
  stack loads or stores: only paired array loads and the final output stores.
  On-device self-test reported zero scalar and bulk mismatches.
- The staged version was slower than both the C path and the existing v1 ASM
  on every span (M useful FLOP/s; 240 MHz, timing multiplier 4):

  | Span | Pair48 C | ASM v1 | ASM staged | Staged/C | Staged checksum rel.err vs Accurate48 |
  |---|---:|---:|---:|---:|---:|
  | 1 | 11.148 | 10.864 | 9.862 | 0.885x | 2.413e-14 |
  | 2 | 11.123 | 10.601 | 9.655 | 0.868x | 1.110e-14 |
  | 3 | 11.056 | 9.871 | 9.123 | 0.825x | 0.000e+00 |

- Removed the unsuccessful staged routine and its Kernel 3 benchmark hook.
  Kernel 3 remains on Pair48 C; its general dot assembly primitive remains
  available in the library. The next whole-kernel experiment is Livermore
  Kernel 1, whose hydro expression reuses exact constants and accesses
  consecutive z elements, making it a better candidate for an S3 paired-load
  loop.

### Whole-loop Livermore Kernel 1 hydro experiment

- Added `pair48_k1_hydro_asm(x, y, z10, n, passes)`. It owns both loops and
  uses paired `ee.ldf.64.ip` / `ee.stf.64.ip` transfers plus `loopnez`.
  Kernel 1's coefficients (`0.125`, `0.25`, `0.5`) are exact FP32 values with
  zero Pair48 low words; the assembly scales the two words directly for the
  coefficient products, then uses the same Pair48 add/multiply grouping for
  the inner sum, multiplication by `y[k]`, and addition of Q.
- The first device run found the overlapping access pattern in `z[k+10]` and
  `z[k+11]`: two post-increment loads advanced by two Pair48 elements, but
  adjacent iterations should advance by one. Added an 8-byte pointer rewind
  after the second z load. The corrected whole-loop assembly then matched the
  Accurate48 checksum exactly on all three spans.
- The first C comparison was invalid: because each pass recomputes the same
  `x[k]` values from unchanged y/z, GCC eliminated redundant C passes. Added a
  compiler memory barrier at each Kernel 1 pass boundary in the parameterized
  C loop so C executes the same declared pass count as assembly. This changes
  no arithmetic and makes the benchmark count valid for this loop-invariant
  kernel.
- ESP-IDF 6.1 build and ESP32-S3 run succeeded; library self-test remained at
  zero scalar and bulk mismatches. With the equivalent pass count (M useful
  FLOP/s; 240 MHz, timing multiplier 4):

  | Span | Pair48 C | Whole K1 ASM | ASM/C | ASM checksum rel.err vs Accurate48 |
  |---|---:|---:|---:|---:|
  | 1 | 14.619 | 14.798 | 1.012x | 0.000e+00 |
  | 2 | 14.602 | 14.776 | 1.012x | 0.000e+00 |
  | 3 | 14.537 | 14.710 | 1.012x | 0.000e+00 |

- The first whole-kernel result is a repeatable ~1.2% gain across these spans.
  Retain Kernel 1 assembly as an optional optimized path. Kernel 3 remains on
  Pair48 C because neither existing nor staged dot assembly beat it.

## Complete Livermore 1–14 results after Kernel 1 timing correction

ESP32-S3, 240 MHz, ESP-IDF 6.1, timing multiplier 4. Rates are M useful
FLOP/s. `Pair48 rel.err` is the relative checksum difference between Pair48 C
and Accurate48. Kernel 1's C pass loop includes the memory barrier described
above so GCC executes the same pass count as the other implementations.

### Span set 1

| Kernel | n | FP32 | Pair48 C | Accurate48 | Pair48 rel.err |
|---|---:|---:|---:|---:|---:|
| 1 Hydro fragment | 1001 | 66.323 | 14.619 | 7.180 | 0 |
| 2 ICCG excerpt | 101 | 61.565 | 15.378 | 7.301 | 0 |
| 3 Inner product | 1001 | 95.586 | 11.148 | 6.069 | 1.413e-13 |
| 4 Banded equations | 1001 | 52.789 | 11.371 | 6.132 | 0 |
| 5 Tri-diagonal | 1001 | 29.685 | 9.401 | 5.513 | 1.627e-15 |
| 6 Linear recurrence | 64 | 56.392 | 10.566 | 5.748 | 1.729e-16 |
| 7 Equation of state | 995 | 1900.896 | 13.901 | 9.790 | 2.097e-16 |
| 8 ADI integration | 100 | 84.131 | 12.747 | 8.274 | 1.560e-16 |
| 9 Integrate predictors | 101 | 12362.400 | 14.348 | 8.319 | 2.527e-16 |
| 10 Difference predictors | 101 | 80.484 | 15.083 | 4.948 | 0 |
| 11 First sum | 1001 | 23.926 | 7.727 | 4.359 | 6.073e-14 |
| 12 First difference | 1000 | 33.380 | 7.609 | 4.320 | 5.181e-11 |
| 13 2-D PIC | 64 | 22.934 | 8.971 | 4.878 | 6.840e-11 |
| 14 1-D PIC | 1001 | 8.509 | 3.413 | 2.457 | 7.463e-13 |

### Span set 2

| Kernel | n | FP32 | Pair48 C | Accurate48 | Pair48 rel.err |
|---|---:|---:|---:|---:|---:|
| 1 Hydro fragment | 101 | 65.986 | 14.602 | 7.175 | 0 |
| 2 ICCG excerpt | 101 | 61.587 | 15.380 | 7.302 | 0 |
| 3 Inner product | 101 | 93.604 | 11.125 | 6.062 | 5.723e-14 |
| 4 Banded equations | 101 | 48.590 | 10.977 | 5.969 | 0 |
| 5 Tri-diagonal | 101 | 29.700 | 9.378 | 5.503 | 9.225e-16 |
| 6 Linear recurrence | 32 | 53.211 | 10.462 | 5.717 | 1.212e-15 |
| 7 Equation of state | 101 | 18961.067 | 13.890 | 9.782 | 3.874e-16 |
| 8 ADI integration | 100 | 84.127 | 12.748 | 8.275 | 1.560e-16 |
| 9 Integrate predictors | 101 | 14422.800 | 14.349 | 8.318 | 2.527e-16 |
| 10 Difference predictors | 101 | 80.517 | 15.084 | 4.949 | 0 |
| 11 First sum | 101 | 23.315 | 7.688 | 4.345 | 5.105e-15 |
| 12 First difference | 100 | 32.712 | 7.538 | 4.299 | 5.329e-11 |
| 13 2-D PIC | 32 | 22.824 | 8.955 | 4.875 | 2.723e-08 |
| 14 1-D PIC | 101 | 8.500 | 3.411 | 2.455 | 1.851e-11 |

### Span set 3

| Kernel | n | FP32 | Pair48 C | Accurate48 | Pair48 rel.err |
|---|---:|---:|---:|---:|---:|
| 1 Hydro fragment | 27 | 64.858 | 14.537 | 7.163 | 0 |
| 2 ICCG excerpt | 15 | 45.451 | 14.250 | 7.035 | 0 |
| 3 Inner product | 27 | 87.945 | 11.054 | 6.041 | 1.279e-14 |
| 4 Banded equations | 27 | 38.440 | 9.862 | 5.496 | 0 |
| 5 Tri-diagonal | 27 | 29.002 | 9.305 | 5.478 | 8.815e-16 |
| 6 Linear recurrence | 8 | 39.282 | 9.877 | 5.525 | 5.202e-16 |
| 7 Equation of state | 21 | 53760.000 | 13.840 | 9.754 | 0 |
| 8 ADI integration | 14 | 80.806 | 12.694 | 8.251 | 4.303e-16 |
| 9 Integrate predictors | 15 | 53040.000 | 14.299 | 8.302 | 2.777e-16 |
| 10 Difference predictors | 15 | 79.121 | 14.965 | 4.938 | 0 |
| 11 First sum | 27 | 21.807 | 7.534 | 4.297 | 4.379e-15 |
| 12 First difference | 26 | 29.129 | 7.332 | 4.233 | 1.196e-12 |
| 13 2-D PIC | 8 | 22.454 | 8.900 | 4.854 | 2.808e-08 |
| 14 1-D PIC | 27 | 8.471 | 3.406 | 2.453 | 8.999e-11 |

### Suite aggregates and optional Kernel 1 assembly

The geometric means over all 42 kernel/span samples were 95.886 Mflop/s for
FP32, 10.373 Mflop/s for Pair48, and 5.740 Mflop/s for Accurate48. As the
Livermore suite defines a performance range, these aggregates do not replace
the per-kernel/span results above.

| Span | Kernel 1 Pair48 C | Whole Kernel 1 ASM | ASM/C | ASM checksum rel.err vs Accurate48 |
|---|---:|---:|---:|---:|
| 1 | 14.619 | 14.798 | 1.012x | 0 |
| 2 | 14.602 | 14.776 | 1.012x | 0 |
| 3 | 14.537 | 14.710 | 1.012x | 0 |

The complete ESP32-S3 run reported zero scalar and zero bulk Pair48 library
self-test mismatches. The whole-loop Kernel 1 routine was checksum-validated
against Accurate48 on all three spans. Kernel 3's C implementation remains
the selected path; the staged dot assembly passed correctness and had no
temporary stack spills, but measured below Pair48 C on all spans and was
removed from the source tree.

## Universal pass barriers, K1 sliding loads, and K13 index trace

The previous complete-suite table above is superseded by this run. The
Kernel 1-only barrier was generalized to `LFK_PASS_BARRIER()` at the end of
each pass in all 14 kernels. The empty volatile memory-clobber asm emits no
target instruction; it prevents GCC from merging or eliminating declared
repetitions. All three generated variants (FP32, Pair48, Accurate48) use the
same barriers.

Kernel 1 assembly now retains the prior `0.25*z[k+11]` Pair48 and obtains the
next `0.125*z[k+11]` by exact multiplication by `0.5`. It loads only the new
`z[k+12]` Pair48 for each following element, replacing the repeated overlapping
load and pointer rewind. This preserves the result for the normal benchmark
inputs, which are exactly scaled by powers of two. The on-device checksum
still matches Accurate48 exactly; whole-kernel throughput improved from about
14.8 to about 15.3 Mflop/s.

ESP32-S3, 240 MHz, ESP-IDF 6.1, timing multiplier 4. Rates are M useful
FLOP/s. Pair48 error is the relative checksum error against Accurate48.

### Span set 1

| Kernel | n | FP32 | Pair48 C | Accurate48 | Pair48 rel.err |
|---|---:|---:|---:|---:|---:|
| 1 Hydro fragment | 1001 | 66.354 | 14.619 | 7.180 | 0 |
| 2 ICCG excerpt | 101 | 61.456 | 15.371 | 7.299 | 0 |
| 3 Inner product | 1001 | 95.460 | 11.145 | 6.068 | 1.413e-13 |
| 4 Banded equations | 1001 | 52.789 | 11.369 | 6.132 | 0 |
| 5 Tri-diagonal | 1001 | 34.203 | 9.402 | 5.513 | 1.627e-15 |
| 6 Linear recurrence | 64 | 56.326 | 10.569 | 5.748 | 1.729e-16 |
| 7 Equation of state | 995 | 147.067 | 13.906 | 9.790 | 2.097e-16 |
| 8 ADI integration | 100 | 84.958 | 12.861 | 8.229 | 1.560e-16 |
| 9 Integrate predictors | 101 | 101.207 | 14.347 | 8.319 | 2.527e-16 |
| 10 Difference predictors | 101 | 51.127 | 15.486 | 4.993 | 0 |
| 11 First sum | 1001 | 23.835 | 7.732 | 4.359 | 6.073e-14 |
| 12 First difference | 1000 | 31.873 | 7.602 | 4.319 | 5.181e-11 |
| 13 2-D PIC | 64 | 24.234 | 8.964 | 4.876 | 6.840e-11 |
| 14 1-D PIC | 1001 | 8.509 | 3.413 | 2.456 | 7.463e-13 |

### Span set 2

| Kernel | n | FP32 | Pair48 C | Accurate48 | Pair48 rel.err |
|---|---:|---:|---:|---:|---:|
| 1 Hydro fragment | 101 | 66.202 | 14.602 | 7.176 | 0 |
| 2 ICCG excerpt | 101 | 61.465 | 15.370 | 7.300 | 0 |
| 3 Inner product | 101 | 92.592 | 11.119 | 6.060 | 5.723e-14 |
| 4 Banded equations | 101 | 48.590 | 10.988 | 5.971 | 0 |
| 5 Tri-diagonal | 101 | 33.652 | 9.380 | 5.503 | 9.225e-16 |
| 6 Linear recurrence | 32 | 53.211 | 10.462 | 5.718 | 1.212e-15 |
| 7 Equation of state | 101 | 143.138 | 13.889 | 9.781 | 3.874e-16 |
| 8 ADI integration | 100 | 84.963 | 12.859 | 8.227 | 1.560e-16 |
| 9 Integrate predictors | 101 | 101.462 | 14.348 | 8.318 | 2.527e-16 |
| 10 Difference predictors | 101 | 51.022 | 15.484 | 4.992 | 0 |
| 11 First sum | 101 | 23.294 | 7.678 | 4.339 | 5.105e-15 |
| 12 First difference | 100 | 30.874 | 7.543 | 4.298 | 5.329e-11 |
| 13 2-D PIC | 32 | 24.137 | 8.957 | 4.876 | 2.723e-08 |
| 14 1-D PIC | 101 | 8.499 | 3.410 | 2.455 | 1.851e-11 |

### Span set 3

| Kernel | n | FP32 | Pair48 C | Accurate48 | Pair48 rel.err |
|---|---:|---:|---:|---:|---:|
| 1 Hydro fragment | 27 | 65.032 | 14.537 | 7.161 | 0 |
| 2 ICCG excerpt | 15 | 44.698 | 14.210 | 7.020 | 0 |
| 3 Inner product | 27 | 84.683 | 10.995 | 6.024 | 1.279e-14 |
| 4 Banded equations | 27 | 38.481 | 9.862 | 5.496 | 0 |
| 5 Tri-diagonal | 27 | 32.311 | 9.305 | 5.475 | 8.815e-16 |
| 6 Linear recurrence | 8 | 39.571 | 9.916 | 5.530 | 5.202e-16 |
| 7 Equation of state | 21 | 126.867 | 13.832 | 9.750 | 0 |
| 8 ADI integration | 14 | 81.440 | 12.809 | 8.204 | 4.303e-16 |
| 9 Integrate predictors | 15 | 98.633 | 14.302 | 8.303 | 2.777e-16 |
| 10 Difference predictors | 15 | 49.678 | 15.267 | 4.969 | 0 |
| 11 First sum | 27 | 21.441 | 7.506 | 4.285 | 4.379e-15 |
| 12 First difference | 26 | 27.986 | 7.366 | 4.230 | 1.196e-12 |
| 13 2-D PIC | 8 | 23.559 | 8.894 | 4.856 | 2.808e-08 |
| 14 1-D PIC | 27 | 8.465 | 3.404 | 2.452 | 8.999e-11 |

The corrected geometric means over 42 samples are **46.294 Mflop/s FP32**,
**10.396 Mflop/s Pair48**, and **5.739 Mflop/s Accurate48**. The physically
implausible FP32 rates for K7 and K9 disappeared once every declared pass was
forced to execute.

### Kernel 1 whole-loop assembly with sliding z window

| Span | Pair48 C | Whole K1 ASM | ASM/C | ASM checksum rel.err vs Accurate48 |
|---|---:|---:|---:|---:|
| 1 | 14.619 | 15.366 | 1.051x | 0 |
| 2 | 14.602 | 15.339 | 1.050x | 0 |
| 3 | 14.537 | 15.246 | 1.049x | 0 |

### Kernel 13 Pair48 vs Accurate48 index trace

Added a post-benchmark trace that runs K13 independently in Pair48 and
Accurate48 and compares each particle-pass's first field lookup, second field
lookup, and final histogram cell. It does not run in the timed benchmark path.

| Span | Particle-passes | First lookup mismatches | Second lookup mismatches | Deposit-cell mismatches | First divergence (pass, particle) |
|---|---:|---:|---:|---:|---|
| 1 | 9216 | 0 | 1 | 1 | (19, 34) |
| 2 | 10496 | 2 | 2 | 2 | (193, 11) |
| 3 | 7936 | 1 | 3 | 3 | (290, 2) |

First divergence details are the tuple sequence `(first lookup) -> (second
lookup) -> (deposit cell)`:

- Span 1, pass 19, particle 34: Pair48 `(36,48)->(36,47)->(37,48)`;
  Accurate48 `(36,48)->(35,47)->(36,48)`.
- Span 2, pass 193, particle 11: Pair48 `(28,58)->(28,58)->(29,59)`;
  Accurate48 `(28,59)->(28,58)->(29,59)`.
- Span 3, pass 290, particle 2: Pair48 `(34,53)->(34,52)->(35,53)`;
  Accurate48 `(34,53)->(33,52)->(34,53)`.

These trace results confirm that K13 does have occasional integer path
divergences. The checksum differences should therefore not be read as pure
floating-point accumulation error. The full build and ESP32-S3 run succeeded;
the Pair48 scalar and bulk library self-test again reported zero mismatches.

## Assembly optimization policy and provisional Cray-1S comparison

### Assembly policy

Assembly instruction-schedule tuning is frozen unless profiling reveals a
clear structural opportunity. Pair48 C `-O3` is the default for Livermore
kernels. Keep the whole-loop Kernel 1 assembly as an optional path: its
sliding `z` window gives a repeatable ~5% gain. Kernel 3 remains Pair48 C;
multiple hand-scheduled dot-product assembly versions lost to GCC. Keep the
generic arithmetic assembly library for correctness checks, bulk operations,
and future experiments. Do not make assembly the default merely because a
kernel is a natural arithmetic match.

### Historical data source and comparison method

For a historical per-kernel reference, use the Cray-1S CFT column in Figure 14
of Jouppi, Bertoni, and Wall, DEC WRL Research Report 89/8 (July 1989),
[*A Unified Vector/Scalar Floating-Point Architecture*](https://www.bitsavers.org/pdf/dec/tech_reports/WRL-89-8.pdf).
It reports the uniprocessor Livermore Loops in MFLOPS and identifies which
loops were vectorized on the Cray. The table cites prior Cray data as
references [5] and [12]. These are Cray-1S results, not an exact Cray-1 model
result. Netlib describes LFK as a 24-loop benchmark and cautions that its value
lies in the per-loop performance range rather than a single mean:
https://www.netlib.org/benchmark/top500/reports/report94/benrep3/node10.html.

For each ESP32-S3 kernel below, the local figure is the geometric mean of its
three span-set Pair48 C measurements (or Accurate48 measurements); this avoids
choosing a favorable span. `Cray / ESP` is the historical Cray-1S rate divided
by the ESP rate. This is a descriptive ratio only, not a controlled benchmark
comparison: the local port preserves the first 14 loop structures, span sets,
IPASS/MUL and FLOP accounting, but uses simplified deterministic initialization
and is explicitly not checksum-compatible with the original LFK driver.
Historical Figure 14 does not state its precision/working-set details in the
figure itself. Treat every row as provisional until the original LFK driver,
initialization, precision, and timing conventions are reproduced locally.

| Kernel | Cray-1S MFLOPS | ESP Pair48 C MFLOPS (3-span GM) | Cray/Pair48 | ESP Accurate48 MFLOPS (3-span GM) | Cray/Accurate48 |
|---:|---:|---:|---:|---:|---:|
| 1 | 68.4 | 14.586 | 4.69x | 7.172 | 9.54x |
| 2 | 16.4 | 14.974 | 1.10x | 7.205 | 2.28x |
| 3 | 63.1 | 11.086 | 5.69x | 6.051 | 10.43x |
| 4 | 20.6 | 10.720 | 1.92x | 5.860 | 3.52x |
| 5 | 5.3 | 9.362 | 0.57x | 5.497 | 0.96x |
| 6 | 6.6 | 10.312 | 0.64x | 5.665 | 1.17x |
| 7 | 82.1 | 13.876 | 5.92x | 9.774 | 8.40x |
| 8 | 65.6 | 12.843 | 5.11x | 8.220 | 7.98x |
| 9 | 80.4 | 14.332 | 5.61x | 8.313 | 9.67x |
| 10 | 28.1 | 15.412 | 1.82x | 4.985 | 5.64x |
| 11 | 4.4 | 7.638 | 0.58x | 4.328 | 1.02x |
| 12 | 21.8 | 7.503 | 2.91x | 4.282 | 5.09x |
| 13 | 4.1 | 8.938 | 0.46x | 4.869 | 0.84x |
| 14 | 7.3 | 3.409 | 2.14x | 2.454 | 2.97x |

Ratios below 1.0x mean the ESP32-S3 measured faster than the published Cray-1S
rate for that row. This surprising result occurs in kernels 5, 6, 11, and 13
for Pair48, and kernels 5 and 13 for Accurate48; because the workload/data and
measurement provenance differ, it must not be presented as evidence that the
ESP32-S3 beats the Cray-1S on the official Livermore benchmark. It is a useful
lead for a fidelity-matched rerun.

The Cray table's 24-kernel scope and warning against reducing performance to a
single mean also matter: this project currently reports only kernels 1-14.
Do not compare the ESP32-S3 42-sample geometric means directly with the
Cray-1S 24-kernel harmonic mean. The useful result at this stage is the
kernel-by-kernel spread above, with the port limitation attached.

Follow-up: this historical comparison and the decision to stop instruction-
schedule tuning are now recorded here as the current project baseline. Use
Pair48 C `-O3` as the default result; report Kernel 1 assembly separately as an
optional optimized result, and do not substitute it into the table above.

### Historical interpretation and fidelity work

Figure 14 marks these first 14 Cray-1S kernels as vectorized:
1, 2, 3, 4, 6, 7, 8, 9, 10, and 12. Kernels 5, 11, 13, and 14 are unmarked
(not vectorized). Three of the four Pair48 rows with a Cray/ESP ratio below 1
are among those non-vectorized kernels (5, 11, 13); K6 is the exception and is
marked vectorized. This pattern is consistent with scalar/dependency-heavy
work reducing the Cray's vector advantage, but the present simplified port
does not prove that causal explanation.

At Accurate48 rates, K5 is 5.497 vs Cray-1S 5.3 MFLOPS, K11 is 4.328 vs 4.4,
and K13 is 4.869 vs 4.1. Treat these as near-parity/provisional observations,
not verified wins, because the local benchmark does not reproduce the original
LFK initialization and checksum driver. The historical Figure 14 reports
Cray-1S harmonic means of 14.4 MFLOPS for loops 1-12, 5.6 for loops 13-24,
and 8.0 for loops 1-24. These harmonic means must not be compared directly
with this port's 42-sample geometric means.

Safe current summary: on a structurally faithful but not checksum-compatible
port of LFK 1-14, Pair48 is within about 10% of the published Cray-1S CFT rate
on K2, exceeds the published rate on K5, K6, K11, and K13, and is generally
about 2-6x behind on the other reported kernels. This is provisional; it is
not an official LFK result. Do not infer that Cray advantage is caused by
vectorization alone from this comparison. The next substantial validation
step is to port the original LFK initialization/checksum machinery and match
the historical precision, run, and timing conventions before drawing stronger
conclusions. Search for independent published Cray-1 (rather than Cray-1S)
tables as a secondary historical reference band.

### Original LFK initialization/checksum fidelity pass (2026-10-01)

Started replacing the simplified deterministic setup with the original Netlib
mf528 `SIGNEL`, special Kernel 13 particle initialization, Kernel 14
`IQRANF` stream, pass-boundary restores, and `SUMO` checksum slices. Added the
original `SUMS(:,span,MULTI=1)` values as checksum validation targets and
printed both the Accurate48 checksum and target so disagreements are
diagnosable. Set the default timing multiplier to 1 for the published
MULTI=1 targets. K1 ASM was disabled in this mode because that implementation
still contains constants from the earlier simplified input setup.

The first board run exposed and corrected three fidelity errors:

* Kernel 2 had a one-element error in the translated output index and used an
  extra final tree stage. It now follows the source's `IPNTP+1` starting
  index and `II.GT.1` repeat condition. Accurate48 checksums now match the
  published target for all three spans.
* Kernel 4's official checksum is `SUMO(V,3)`, not a weighted checksum of the
  three modified aliased `XZ` elements. Added the original V checksum values;
  Accurate48 now matches all three targets.
* Kernel 13's initial P values follow the original `j=1..4`, then `k=1..512`
  DS/DW loop. Corrected the flattened layout from `4*i+j` to `j*512+i`.

The current board build succeeds and the Pair48 scalar/bulk self-test remains
at zero mismatches. The Accurate48 driver checksum now matches original
`SUMS` on K1-K9 excluding K10, and K11-K12 closely (K12 differs by up to
about 2.4e-10 relative). K14 differs by up to about 1.35e-3 relative. K10
and K13 remain substantially different (K10's span-3 observed checksum was
-1.966253912e3 versus 1.651291228e3; K13 was 9.926795581e5 versus
4.143805389e7). These are
unresolved validation failures, so current throughput figures must not be
presented as fidelity-validated LFK performance. Keep the full actual/target
checksum display and investigate the K10 input/buffer mapping and K13 PIC
state/checksum layout before claiming the driver matches the historical run.

The local run still uses one test run rather than the standard seven complete
runs and does not reproduce the original host timing calibration; the board
uses Accurate48 as a software precision mode rather than Cray hardware. Even
after checksum mismatches are resolved, label these as a port validation and
not an official benchmark result unless those run/timing details are also
matched.

### Secondary Cray-1 reference table

Found independent Cray-1 (not Cray-1S) per-kernel CFT '84 values in
Datamation, September 1984, Fig. 3, “Some benchmarks using the Livermore
kernels”: 100.0, 41.7, 33.3, 24.3, 7.7, 7.0, 120.0, 55.4, 68.0, 36.0,
2.9, 25.0, 4.0, and 5.6 MFLOPS for kernels 1-14. A dissertation table
reproduces those same values and attributes them to [Worl84; Wolc91], with
the Cray compiler listed as CFT '84. The Datamation comparison includes
multiple machines and compiler columns; keep these as a separate historical
reference band and verify precision/run conventions before combining them
with the Cray-1S data. Sources: [Datamation, 1984-09-01](https://www.bitsavers.org/magazines/Datamation/19840901.pdf),
[Wolcott dissertation, Table 5-5](https://studylib.net/doc/25884033/p.wolcott).

### Follow-up audit before repeat-state correction (2026-10-01)

Reviewed the proposed Fortran-order checksum issue against the actual array
shapes. K10 declares `px[101][25]`, so the contiguous word at linear index
`lin` is `px[lin/25][lin%25]`, the same physical order as Fortran `PX(j,k)`.
K13 declares `p[512][4]`, mapping linear word `lin` to
`p[lin/4][lin%4]` (= Fortran `P(j,k)`). Its H array is deliberately accessed
as `h[j][i]`; linear `h[lin/64][lin%64]` maps to Fortran `H(i,j)`. Therefore
the existing contiguous `SUMO` slices already walk physical Fortran storage;
changing them to a logical row gather would be wrong and would not explain the
remaining checksum gaps. Added comments at the checksum cases documenting
these index mappings. Confirmed the K13 pass restore copies all 64x64 H words
from H0 and restores four P components for each active particle.

Added a check against the 30-value IQRANF example embedded in Netlib. The
integer Park-Miller implementation produces the documented vector exactly,
and firmware prints `IQRANF seed-256 reference vector: PASS`.

K14 now converts Pair48/Accurate48 integers from the reconstructed `hi+lo`
value, rather than truncating `hi` alone; the latter could cross an integer
boundary incorrectly. Also restored the Fortran left-associative evaluation
of `VX = VX + EX1 + (XX-XI)*DEX1`. Build and board run succeeded. The K14
checksum errors remain about 4.34e-7, 7.63e-5, and 1.35e-3 over spans 1-3,
so these changes did not remove the larger span-dependent discrepancy.

Latest board run still has K10 and K13 checksum failures. K10 Accurate48
checksums were -9.413714349e4, -9.413714349e4, and -1.966253912e3 versus
targets 7.310369784e4, 7.310369784e4, and 1.651291228e3. K13 was
6.712639175e7, 1.609608794e7, and 9.926795581e5 versus targets
4.468741170e8, 2.323352390e8, and 4.143805389e7. Continue from the original
Netlib C `TEST`/`VALUES` layouts; do not infer a checksum traversal bug from
these differences alone. Performance remains provisional.

### Earlier elementwise audit and source cleanup, superseded for stateful kernels (2026-10-01)

Added an elementwise audit to the canonical `components/livermore` firmware.
It reruns each kernel outside the timed section, captures exactly the arrays
used by that kernel's checksum, and reports checksum-relative error,
representation and numeric difference counts, maximum absolute/relative error,
and the worst flattened slice index with Pair48 high/low deltas. The audit
covers all 14 kernels at all three spans. An index of -1 means no numeric
difference was found in the slice.

Flashed and ran the final build on the ESP32-S3 (240 MHz, ESP-IDF 6.1,
2026-10-01). Pair48 scalar and bulk self-tests both report zero mismatches;
the seed-256 IQRANF vector passes. The audit verifies that equal aggregate
checksums do not imply equal results. For example, K1 span 1 has checksum
relative error 4.268e-16 yet 183 numerically different outputs (415 distinct
hi/lo representations), with maximum relative element error 4.354e-15. K9
span 1 has exactly zero checksum difference but 26 numerically different
outputs. K10 spans 1 and 2 also have exactly zero Pair48/Accurate48 checksum
difference despite 221 numeric element differences and max relative element
error 2.620e-9. Conversely, K4 is exactly identical across its three checksum
slices for all spans. Thus checksum agreement remains a useful aggregate
check, but not an elementwise precision guarantee.

At that pre-correction point, the run showed official checksum mismatches in
K10/K13 and a span-dependent K14 result. The later LFK repeat-state correction
below resolves those Accurate48 target mismatches. The earlier K13 index audit
also reset particle state per iteration; the corrected run's Pair48 index
paths can diverge from Accurate48 at rounding-sensitive boundaries. See the
current results entry below and `BENCHMARK_FINDINGS.md`.

Source tree cleanup removed duplicate/stale Livermore copies from `main/` and
old packaged snapshots from `main/asm/`. The active source of truth is
`components/livermore/`; the reusable Pair48 library is `main/asm/`. Updated
CMake/Kconfig comments and project notes point to those canonical paths. A
complete source-only archive is generated at the project root and excludes
build products. Reason: the former partial archive omitted the separately
registered Livermore component, while stale copies and nested archives could
be mistaken for active code.

Complete element audit table from this board run (format: kernel, span,
checksum-relative error, differing representations, differing numeric values,
maximum absolute error, maximum relative error, worst flattened index):

```text
 1 1 4.268e-16 415 183 4.441e-16 4.354e-15 990
 2 1 5.907e-16  71  54 8.882e-16 2.372e-14 196
 3 1 3.436e-13   1   1 3.439e-12 3.436e-13   0
 4 1 0.000e+00   0   0 0.000e+00 0.000e+00  -1
 5 1 0.000e+00 484 268 1.249e-16 9.370e-10   0
 6 1 1.102e-14  58  55 2.842e-13 7.523e-14  56
 7 1 1.192e-16 604 448 1.221e-15 9.913e-15 419
 8 1 1.939e-16 135 124 6.661e-16 6.662e-15 1901
 9 1 0.000e+00  37  26 1.332e-15 7.013e-15 675
10 1 0.000e+00 310 221 6.217e-15 2.620e-09 1505
11 1 8.882e-14 989 970 9.868e-11 1.067e-12 923
12 1 4.397e-08 999 662 2.220e-16 1.464e-09 942
13 1 2.220e-16  48  48 1.819e-12 3.497e-15  24
14 1 4.520e-16 1568 1015 1.019e-10 6.245e-10 2040
 1 2 0.000e+00  50  41 4.441e-16 4.354e-15  49
 2 2 5.907e-16  71  54 8.882e-16 2.372e-14 196
 3 2 1.407e-14   1   1 1.421e-14 1.407e-14   0
 4 2 0.000e+00   0   0 0.000e+00 0.000e+00  -1
 5 2 0.000e+00  54  15 8.327e-17 9.370e-10   0
 6 2 2.140e-15  26  24 7.355e-15 3.087e-14  27
 7 2 1.792e-16  53  44 4.441e-16 3.605e-15  72
 8 2 1.939e-16 135 124 6.661e-16 6.662e-15 1901
 9 2 0.000e+00  37  26 1.332e-15 7.013e-15 675
10 2 0.000e+00 310 221 6.217e-15 2.620e-09 1505
11 2 7.417e-15  89  78 2.984e-13 3.280e-14  89
12 2 5.861e-09  99  60 2.220e-16 1.463e-09  68
13 2 0.000e+00  24  24 1.819e-12 3.497e-15  24
14 2 1.245e-16 164 108 9.095e-11 9.029e-12 244
 1 3 3.686e-16  13  10 3.331e-16 3.266e-15   3
 2 3 5.392e-16   5   5 4.441e-16 5.552e-15  22
 3 3 6.581e-15   1   1 1.776e-15 6.581e-15   0
 4 3 0.000e+00   0   0 0.000e+00 0.000e+00  -1
 5 3 2.791e-16  11   3 8.327e-17 9.370e-10   0
 6 3 3.964e-16   5   4 1.943e-16 8.030e-15   3
 7 3 4.994e-16   7   7 2.220e-16 1.802e-15  10
 8 3 0.000e+00   0   0 0.000e+00 0.000e+00  -1
 9 3 0.000e+00   5   3 8.882e-16 4.675e-15   0
10 3 2.313e-16  53  37 5.329e-15 2.617e-09 155
11 3 8.677e-16  15  13 5.329e-15 3.230e-15   9
12 3 2.856e-10  25  13 1.110e-16 7.316e-10  20
13 3 2.345e-16   7   7 1.819e-12 3.497e-15  24
14 3 1.826e-15  49  40 5.821e-11 3.687e-11  75
```

The printed `hi_diff`/`lo_diff` were also available on the board for each row;
they are signed Pair48-minus-Accurate48 component differences at that row's
worst element. The complete raw table is preserved in this entry by the
summary metrics above, and can be regenerated by running the instrumented
firmware.


### Findings file organization (2026-10-01)

Benchmark summaries and historical comparison tables are maintained in the
project-root `BENCHMARK_FINDINGS.md`; this file retains chronological
assembler/source changes and detailed validation evidence. Keep numerical
claims, dates, and caveats synchronized between the two documents.


The earlier pre-correction audited run reported geometric means FP32 32.773,
Pair48 8.100, and Accurate48 4.748 MFLOPS over 42 kernel/span samples. Its
stateful-kernel results were superseded by the LFK repeat-state correction
below. The concise current summary is kept in `BENCHMARK_FINDINGS.md`, while
this log retains the element-audit details.


### LFK repeat-state correction and board validation (2026-10-01)

Found the source of the remaining checksum mismatches: the C port restored
K10 predictor inputs, K13 particle/histogram state, and K14 histogram bins on
every inner iteration. Netlib mf528 sets `Loop = IPASS * MUL * MULTI`; these
are consecutive stateful executions, and the LFK TEST routine only restores
state at an outer timing-repeat boundary. With one outer repeat, there is no
restore inside these loops. Removed the per-iteration restores from K10, K13,
and K14 and aligned the K13 index-path diagnostic with that execution model.
The accurate checksum comparisons were not being checked against a different
layout; the translated computation had been changed by those restores.

A host binary64 instantiation of the same loop source with those restores
removed reproduced all 9 published K10/K13/K14 span checksums exactly (to
printed precision). Then rebuilt, flashed, and ran the corrected firmware on
the ESP32-S3, 240 MHz, ESP-IDF 6.1. Accurate48 checksums now match all three
spans for each of K10, K13, and K14. Maximum relative checksum differences
for those kernels were 0 for K10, 1.007e-14 for K13, and 6.079e-16 for K14.
All K1-K14 checksums agree with published `SUMS` to Accurate48 precision; K12
is the largest residual due to cancellation in its very small checksum
(1.473e-9, 1.012e-10, 2.356e-10 relative across spans 1-3).

Corrected board run geometric means across 42 samples: FP32 43.555 MFLOPS,
Pair48 9.074 MFLOPS, Accurate48 5.200 MFLOPS. Pair48 library self-tests
remain zero-mismatch; IQRANF reference vector passes. The performance run is
still provisional because only kernels 1-14 and one timing trial are run,
without full seven-trial timing calibration. Pair48's K13 PIC path diverges
from Accurate48 at some rounding-sensitive indices (first-index mismatches
64/32/200 across spans; second-lookups and deposit cells additionally differ
only in span 3, 204 cases). Accurate48 reproduces published K13 checksums,
while Pair48's aggregate checksum remains close; do not treat that checksum as
proof of identical particle paths.

Current corrected element-audit values for the stateful kernels (kernel,
span, checksum-relative Pair48/Accurate48 error, repr differences, numeric
differences, max absolute, max relative, worst index):

```text
10 1 0.000e+00   0    0 0.000e+00 0.000e+00   -1
10 2 0.000e+00   0    0 0.000e+00 0.000e+00   -1
10 3 0.000e+00   0    0 0.000e+00 0.000e+00   -1
13 1 1.601e-15 257  253 3.725e-09 2.165e-13  238
13 2 3.309e-14 129  128 1.770e-08 6.847e-13   62
13 3 1.546e-14  32   32 2.962e-07 1.677e-12    3
14 1 4.520e-16 1568 1014 3.212e-10 1.219e-09 2050
14 2 3.735e-16 164  108 5.305e-10 9.496e-11  258
14 3 1.459e-14  49   40 1.872e-08 9.551e-10   80
```

The preceding same-date K10/K13/K14 failure entries are retained as the
debugging history; this correction supersedes them. Summary performance and
validation status are synchronized in `BENCHMARK_FINDINGS.md` and
`docs/LIVERMORE_PORT.md`.
