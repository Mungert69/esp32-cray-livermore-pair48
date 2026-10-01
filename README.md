# ESP32-S3 vs Cray-1: Pair48 Precision and Livermore Performance

This repository explores how far an ESP32-S3 can run scientific workloads associated with the Cray-1 era using software extended precision. It contains a reusable two-float arithmetic library, hand-written Xtensa assembly experiments, and a fidelity-oriented port of Livermore loops 1–14.

The project separates three questions that are easy to conflate:

1. How fast can the S3 run ordinary FP32 arithmetic?
2. How much does software extended precision cost, and can selected assembly transformations reduce that cost?
3. Does the translated benchmark reproduce the original Livermore workload and checksums closely enough to make performance comparisons meaningful?

The first two have useful measured results. The third has substantially improved: Accurate48 now agrees with the published Livermore checksum targets for loops 1–14 across all three configured spans, within the precision limits described below. This is still a partial port and not an official Livermore benchmark result.

## The comparison at a glance

The headline question is how an ESP32-S3 running software extended precision compares with a real Cray-1 on Livermore workloads. A historical Cray-1 CFT '84 table reports these per-kernel rates ([Datamation, September 1984, Fig. 3](https://www.bitsavers.org/magazines/Datamation/19840901.pdf); reproduced in [Wolcott, Table 5-5](https://studylib.net/doc/25884033/p.wolcott)):

| Livermore kernel | Cray-1 CFT '84 (MFLOPS) |
| ---: | ---: |
| 1 | 100.0 |
| 2 | 41.7 |
| 3 | 33.3 |
| 4 | 24.3 |
| 5 | 7.7 |
| 6 | 7.0 |
| 7 | 120.0 |
| 8 | 55.4 |
| 9 | 68.0 |
| 10 | 36.0 |
| 11 | 2.9 |
| 12 | 25.0 |
| 13 | 4.0 |
| 14 | 5.6 |

For the same first 14 kernel numbers, the corrected ESP32-S3 run measured geometric means across the three configured spans of **9.074 MFLOPS Pair48** and **5.200 MFLOPS Accurate48**. Taking the geometric mean of the 14 published Cray-1 rates gives **21.60 MFLOPS**. That puts the Cray-1's geometric mean at about **2.38× Pair48** and **4.15× Accurate48** in this provisional comparison. FP32 on the ESP32-S3 measured 43.555 MFLOPS geometric mean, but it does not provide comparable precision.

These are useful orientation figures, not official LFK scores or controlled head-to-head results. The Cray-1 table is a single historical rate per kernel; the ESP32-S3 figure combines three span sets and uses one timing trial. The ESP32 port includes only kernels 1–14 and does not reproduce the full historical timing/calibration driver. Historical compiler, precision, and run conventions are not fully matched. Read the detailed caveats and per-kernel historical references in [`BENCHMARK_FINDINGS.md`](BENCHMARK_FINDINGS.md) and [`main/asm/PAIR48_DEV_LOG.md`](main/asm/PAIR48_DEV_LOG.md).

The main takeaway is the wide per-kernel range: the Cray-1 table spans 2.9 to 120 MFLOPS. The aggregate ratios summarize the 14-kernel comparison but do not show that one machine wins every kernel; use the findings document for the historical kernel-by-kernel reference data and their provenance.

## Hardware and toolchain

- ESP32-S3, configured for 240 MHz, dual-core Xtensa LX7, with native FP32 arithmetic.
- ESP-IDF 6.1 project; the active board and configuration details are in `sdkconfig.defaults`.
- Livermore mode uses the `components/livermore/` component. The selectable SAXPY benchmark remains in `main/`.

## Pair48 arithmetic

`pair48_t` stores a value as two FP32 numbers:

```c
typedef struct {
    float hi;
    float lo;
} pair48_t;
```

The represented value is approximately `hi + lo`. The low word retains information lost when the high word is rounded. This provides substantially more precision than a single float, but it is not a fully normalized 48-bit floating-point format: operations do not renormalize after every result, so accuracy depends on the operation sequence and workload. We therefore compare it with the separate Accurate48 implementation and with the original benchmark checksums instead of treating its nominal significand size as an accuracy guarantee.

`main/asm/pair48_asm.h` keeps inline C arithmetic as the reference implementation and exposes pointer-based assembly calls, whole-array add/multiply/SAXPY routines, and vector helpers. The pointer API avoids Xtensa aggregate-return ABI complications. The self-test checks scalar and bulk operations against the C reference. The current development log records zero mismatches on the ESP32-S3.

One earlier standalone SAXPY test measured the assembly routine at 27.16 cycles per element (8.835 million Pair48 elements/s/core at 240 MHz). Its own C comparison was 61.09 cycles/element, but that C measurement is not comparable to a separate previously optimized Pair48 C implementation measured at 31.56 cycles/element. Treat the 27.16-cycle figure as a historical microbenchmark result with that measurement caveat, not as the Livermore suite's performance result.

Assembly remains available as a reusable library and for clear whole-loop transformations. Pair48 C `-O3` is the default for Livermore kernels. Whole-kernel assembly is kept for Kernel 1, where a sliding window reuses the overlapping `z` element and measured about a 5% improvement. Kernel 3 dot-product assembly was tested, including a staged schedule based on GCC output, but did not beat the C implementation and is not selected. The evidence and rationale are in `main/asm/PAIR48_DEV_LOG.md`.

## Livermore loops 1–14

The canonical port is in `components/livermore/`. `livermore_kernels_1_14.inc` contains the shared kernel bodies, setup, repetition behavior, and checksum slices; `livermore_esp32.c` supplies arithmetic modes, board timing, checksum checks, and the Pair48-versus-Accurate48 element audit. `main/livermore_main.c` is the app entry point.

The port incorporates the Netlib `SIGNEL` setup, documented Kernel 13 particle initialization, the seed-256 `IQRANF` test vector, original checksum slices, and stateful repeat behavior. A key correction was removing restores that had been placed inside each repeated kernel execution for K10, K13, and K14. LFK repeats are stateful; restoring those arrays per iteration changed the benchmark. The corrected Accurate48 implementation now agrees with the published `SUMS` targets for K1–K14 on all three spans. K12 has the largest relative checksum residual (approximately `1.47e-9`, `1.01e-10`, and `2.36e-10` over spans 1–3) because its checksum is small and cancellation-sensitive. The K10, K13, and K14 residuals are at most about `1.0e-14`.

The current corrected board run's geometric means across 42 kernel/span samples are:

| Arithmetic mode | Geometric mean |
| --- | ---: |
| FP32 | 43.555 MFLOPS |
| Pair48 | 9.074 MFLOPS |
| Accurate48 | 5.200 MFLOPS |

These are provisional engineering measurements, not official LFK scores. The firmware covers only loops 1–14, uses one timing trial rather than the standard seven-trial/calibration procedure, and does not reproduce every historical compiler, timing, and driver convention. Do not compare these geometric means directly with a historical 24-loop harmonic mean.

The historical comparisons are kept per kernel in `BENCHMARK_FINDINGS.md`. The source records Cray-1S CFT data as a reference set and an independent Cray-1 CFT '84 table as a second reference band. They are context, not an apples-to-apples contest: machine model, compiler, precision, initialization, and timing methods differ. The data do not establish that vectorization alone explains the relative results.

### Numerical interpretation

An equal scalar checksum does not establish equal element results; differences can cancel in the reduction. The firmware therefore also audits Pair48 and Accurate48 values element by element over checksum-contributing slices. In the corrected run, Pair48's K13 particle-in-cell path sometimes chooses different integer cells at rounding-sensitive boundaries. Its aggregate checksum can remain close while the discrete particle path differs. For that kernel, consult the index trace and element audit rather than interpreting checksum agreement as an identical simulation.

## Build and run

Install ESP-IDF 6.1 and its Xtensa toolchain, then from the repository root:

```sh
. "$IDF_PATH/export.sh"
idf.py set-target esp32s3
idf.py menuconfig
```

In **Benchmark Selection**, choose **Livermore Kernels 1-14 benchmark**. Then build and flash (replace the port as needed):

```sh
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

The checked-in `sdkconfig.defaults` selects ESP32-S3, 240 MHz, performance optimization, PSRAM settings, and the single-app partition table. Local `sdkconfig` is intentionally ignored; ESP-IDF generates it for each development environment. Select the Cray SAXPY benchmark in menuconfig to run the alternate benchmark and Pair48 self-tests.

## Repository guide

- `main/asm/` — Pair48 C reference, scalar and bulk Xtensa assembly, vector helpers, self-test, assembly audit, and chronological development log.
- `components/livermore/` — active Livermore 1–14 implementation.
- `main/` — ESP-IDF entry points, benchmark selection, and the alternate SAXPY tests.
- `BENCHMARK_FINDINGS.md` — concise current status, historical per-kernel data, caveats, and pointers to detailed evidence.
- `docs/LIVERMORE_PORT.md` — source-of-truth map and validation notes.
- `main/asm/PAIR48_DEV_LOG.md` — detailed chronological arithmetic/assembly decisions, board measurements, corrections, and elementwise audits.

The development log preserves superseded results as debugging history and labels them accordingly; use the corrected state-repeat run as the current Livermore result. The findings summary, port notes, and README should be kept synchronized when new measurements are added.

## Historical sources and licensing

The port and comparison notes use the Livermore Fortran Kernels source and published checksum data distributed by [Netlib](https://www.netlib.org/benchmark/livermore), plus historical Cray performance material cited in `BENCHMARK_FINDINGS.md` and the development log. Check those sources for the original benchmark's terms and attribution. This repository currently has no `LICENSE` file; public visibility alone does not grant permission to reuse its contents. Preserve upstream notices and determine licensing before redistributing derived benchmark source beyond this repository.
