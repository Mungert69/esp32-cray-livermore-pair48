# ESP32-S3 vs Cray-1 48-bit Benchmark - Findings Summary

> **Current status (2026-10-01):** The LFK 1–14 port now follows stateful
> repeat behavior for K10/K13/K14. Accurate48 agrees with the published
> `SUMS` targets for all 14 kernels across all three spans, with K12 the
> cancellation-sensitive exception at roughly 1.47e-9 relative error on span
> 1. The corrected one-trial board run has 42-sample geometric means of 43.555
> MFLOPS FP32, 9.074 MFLOPS Pair48, and 5.200 MFLOPS Accurate48. These are
> provisional: only 14 of 24 kernels are implemented, and standard timing
> calibration/seven-trial conventions are not reproduced. Pair48 K13 can take
> different rounding-sensitive integer paths from Accurate48. See the README
> for interpretation and `main/asm/PAIR48_DEV_LOG.md` for full details.
>
> The SAXPY measurements below are earlier microbenchmarks, not the current
> Livermore results. They have different measurement setups; in particular,
> the 27.16-cycle assembly result must not be compared with the 61.09-cycle C
> number as if that were the optimized 31.56-cycle Pair48 C baseline.

## Project Overview
- **Target**: ESP32-S3 (dual-core Xtensa LX7 @ 240 MHz, FPU with MADD.S/MSUB.S fused multiply-add)
- **Precision**: Double-word (ds48_t) - 48-bit significand using two float32 (hi/lo)
- **Kernel**: SAXPY (y = a*x + y) for both FP32 and DS48
- **Comparison**: Cray-1 memory-to-memory SAXPY ceiling: 26.67 M elements/s (80M 64-bit words/s / 3 words per element)

## MAJOR BREAKTHROUGH: Pair Arithmetic (TwoFloat-style)

### Performance Results

| Kernel | Cycles/Elem | M elements/s | vs Cray Ceiling |
|--------|-------------|--------------|-----------------|
| FP32 (fma_s) | 14.03 | 17.11 | 1.56x |
| **DS48 accurate u2 piped** | **44.57** | **5.39** | **4.95x** |
| **Pair48 unroll-2 (NEW)** | **31.56** | **7.61** | **3.51x** |
| **Pair48 unroll-4 (NEW)** | **33.81** | **7.10** | **3.76x** |

### Dual-Core Results

| Kernel | Dual-Core M/s | Scaling | vs Cray Ceiling |
|--------|---------------|---------|-----------------|
| DS48 accurate u2 piped | 7.97 | 1.48x | 3.35x |
| **Pair48 unroll-2** | **15.12** | **1.99x** | **1.77x** |
| **Pair48 unroll-4** | **14.03** | **1.98x** | **1.90x** |

### Numerical Accuracy Verification

| Metric | Value |
|--------|-------|
| Max relative error (vs accurate DS48) | 6.8e-12 |
| Max ULP error @ 48-bit | ~1,900 ULP |
| Error after 256 SAXPY passes | ~1e-14 (no growth) |
| FP32 relative error vs DS48 | ~1e-7 (much worse) |

**Key finding**: Pair arithmetic error **does not accumulate** over repeated operations - stays near machine epsilon (~1e-14) even after 256 passes!

## Memory Bottleneck Investigation Results

### Key Finding: FPU Register Dependencies Dominate (Not Memory)

| Test | Single-Core | Dual-Core Scaling | D_STALL | BUBBLES_REG_DEP |
|------|-------------|-------------------|---------|-----------------|
| DS48 unroll-2 | 45.07 cyc/elem (5.33 M/s) | 1.49x | 8-10 | **1.57M** |
| **DS48 u2 piped** | **44.55 cyc/elem (5.39 M/s)** | **1.48x** | 9-19 | **1.44M** |
| Pure SRAM COPY | 5.05 cyc/elem (380 GB/s) | ~0x (bug) | 15 | 771 |
| Pure SRAM TRIAD | 13.04 cyc/elem (442 GB/s) | ~0x (bug) | 13 | 475 |

**Interpretation**:
- **BUBBLES_REG_DEP reduced from 1.57M to 1.44M** over 262K elements = **~6 → ~5.5 bubbles/element**
- **D_STALL = 9-19** = negligible memory stalls (confirmed by 380+ GB/s SRAM bandwidth)
- Immediate scheduling target: 45 - 5.5 ≈ **39.5 cycles/element**
- SRAM bandwidth is NOT the bottleneck (380+ GB/s available)

### Perf Counter Comparison: Unroll-2 vs Piped Kernel

| Kernel | Cycles/Elem | M elements/s | BUBBLES_REG_DEP | Bubbles/Elem | D_STALL |
|--------|-------------|--------------|-----------------|--------------|---------|
| Unroll-2 | 45.05 | 5.33 | 1,573,553 | 6.00 | 9-17 |
| **Piped** | **44.55** | **5.39** | **1,442,591** | **5.51** | 9-19 |

**Reduction**: **8.3% fewer dependency bubbles** → 0.5 cyc/element improvement

## DS48 Accurate u2 Piped Kernel - Key Optimizations

### Strategy: Explicit Software Pipelining
Separates all independent operations into stages to maximize ILP:

```c
/* Stage 1: Load all data (8 floats for 2 elements) */
float x0_hi = x[i].hi, x0_lo = x[i].lo;
float x1_hi = x[i+1].hi, x1_lo = x[i+1].lo;
float y0_hi = y[i].hi, y0_lo = y[i].lo;
float y1_hi = y[i+1].hi, y1_lo = y[i+1].lo;
float a_hi = a.hi, a_lo = a.lo;

/* Stage 2: ALL multiplies (8 independent MUL.S) */
float p0_hi = a_hi * x0_hi;
float p1_hi = a_hi * x1_hi;
float p0_lo_ll = a_lo * x0_lo;
float p1_lo_ll = a_lo * x1_lo;
float p0_lo_lh = a_hi * x0_lo;
float p1_lo_lh = a_hi * x1_lo;
float p0_lo_hl = a_lo * x0_hi;
float p1_lo_hl = a_lo * x1_hi;

/* Stage 3: Error terms (2 FMS) */
float p0_neg_err = fms_s(p0_hi, a_hi, x0_hi);
float p1_neg_err = fms_s(p1_hi, a_hi, x1_hi);

/* Stage 4: Cross-term accumulation (4 FMA) */
p0_lo = fma_s(fma_s(fma_s(p0_lo_ll, a_hi, x0_lo), a_lo, x0_hi), 0, 0) - p0_neg_err;
p1_lo = fma_s(fma_s(fma_s(p1_lo_ll, a_hi, x1_lo), a_lo, x1_hi), 0, 0) - p1_neg_err;

/* Stage 5: FastTwoSum for p.hi + p.lo (4 ops each) */
/* Stage 6: TwoSum for p + y (full expansion) */
```

### Why This Beats Unroll-2/Unroll-4
1. **All 8 multiplies issued before any dependent operation** - hides ~4-cycle FPU latency
2. **No struct copies** - scalars stay in registers
3. **No function call overhead** - inline FMA/FMS
4. **Register pressure manageable** - ~16 live FP values fits in f0-f15

## Pair Arithmetic Implementation

### Pair Multiply (7 FP ops with FMA)
```c
static inline ds48_t pair_mul(ds48_t x, ds48_t y)
{
    float p = x.hi * y.hi;
    float e = fms_s(p, x.hi, y.hi);    /* e = -(x.hi*y.hi - p) */
    float t = x.lo * y.lo;
    t = fma_s(t, x.hi, y.lo);
    t = fma_s(t, x.lo, y.hi);
    t = t - e;
    return (ds48_t){ .hi = p, .lo = t };
}
```

### Pair Add (8 FP ops)
```c
static inline ds48_t pair_add(ds48_t x, ds48_t y)
{
    float s = x.hi + y.hi;
    float v = s - x.hi;
    float lo = (x.hi - (s - v)) + (y.hi - v) + x.lo + y.lo;
    return (ds48_t){ .hi = s, .lo = lo };
}
```

### Operation Count Comparison

| Algorithm | Multiply | Add | SAXPY Total |
|-----------|----------|-----|-------------|
| **Accurate DS48** | 10 | 20 | **~30 FP ops** |
| **Pair Arithmetic** | 7 | 8 | **~15 FP ops** |

**Pair arithmetic is ~2x fewer FP operations** - explains the 29% single-core speedup!

## Dual-Core Wake Ordering Fix (Critical)

```c
// WRONG (serializes):
xTaskNotifyGive(worker0.task);  // Core 0 starts
xTaskNotifyGive(worker1.task);  // Core 1 starts

// CORRECT (parallel):
BaseType_t current_core = xPortGetCoreID();
if (current_core == 0) {
    xTaskNotifyGive(worker1.task);  // Wake OTHER core FIRST
    xTaskNotifyGive(worker0.task);  // Then wake self (preempts)
} else {
    xTaskNotifyGive(worker0.task);
    xTaskNotifyGive(worker1.task);
}
```

**Impact**: Without fix, dual-core scaling = ~0.1x (serial). With fix: 1.48-1.99x.

## Assembly Analysis (xtensa-esp32s3-elf-objdump)

The piped kernel compiles to ~180 instructions in the loop body with good instruction interleaving. Key observations:
- All 8 MUL.S issued consecutively (cycles 0-7)
- FMS/FMA interleaved for both elements
- TwoSum sequences fully unrolled
- No register spilling detected

**Remaining bubbles**: Compiler still emits some dependent sub.s chains. Hand-scheduled assembly could target **39-40 cycles**.

## Theoretical Limits

| Metric | Value |
|--------|-------|
| Cray-1 memory SAXPY ceiling | 26.67 M elements/s |
| ESP32-S3 dual-core best (Pair48 u2) | 15.12 M elements/s |
| **Ratio** | **1.77x** |
| Target with hand-scheduled pair asm (25 cyc) | ~9.6 M/s core → 18 M/s dual = **1.48x** |

## Cray-1 Comparison Summary

| Implementation | Dual-Core M/s | Cray Ratio | Notes |
|----------------|---------------|------------|-------|
| Cray-1 memory ceiling | 26.67 | 1.00x | Reference |
| ESP32-S3 FP32 | 34.22 | 0.78x | But only 24-bit precision |
| ESP32-S3 Accurate DS48 | 7.98 | 3.34x | Full 48-bit, normalized |
| **ESP32-S3 Pair48 u2** | **15.12** | **1.77x** | **48-bit, near-Cray precision** |

**The pair arithmetic kernel brings ESP32-S3 within 1.77x of the Cray-1 memory bandwidth ceiling** - a dramatic improvement over 3.34x!

## Next Steps

1. **Hand-schedule Pair48 unroll-2 in assembly** using `ee.ldf.128.ip` / `ee.stf.128.ip`:
   - Load 2 ds48_t (4 floats) in 1 instruction
   - Store 2 results in 1 instruction
   - Explicit FPU register allocation
   - Target: **25-30 cycles/element**

2. **Periodic renormalization test**: Run pair kernel with renormalize every 8/16/32 passes
   - Check if error stays bounded while keeping most speed

3. **Test with larger N** - verify cache/DRAM behavior

## Build & Run Commands

```bash
# Build
source /home/mahadeva/.espressif/tools/python/v6.1/venv/bin/activate
export IDF_PATH=/home/mahadeva/.espressif/v6.1/esp-idf
. $IDF_PATH/export.sh
cd /home/mahadeva/code/ESP32CrayTest
idf.py build

# Flash
idf.py -p /dev/ttyACM0 flash

# Monitor with reset
python3 -c "
import serial, time
ser = serial.Serial('/dev/serial/by-id/usb-1a86_USB_Single_Serial_5C94135024-if00', 115200, timeout=90)
ser.dtr = False; ser.rts = False; time.sleep(0.2)
ser.dtr = True; ser.rts = True; time.sleep(0.2)
ser.dtr = False; ser.rts = False
for _ in range(500):
    line = ser.readline()
    if line: print(line.decode('utf-8', errors='replace').rstrip())
"
```

## Initial LFK fidelity audit, superseded below (2026-10-01)

An initial attempt to replace simplified inputs with the Netlib mf528
initialization and `SUMO` checksums is in progress. Changes include the
`SIGNEL` recurrence, original Kernel 13 P initialization, Kernel 14 Park-Miller
index stream, timed-pass input restores, Kernel 2's tree loop, and Kernel 4's
`SUMO(V,3)` checksum. The Accurate48 checksums on the S3 now match the
published `SUMS(:,span,MULTI=1)` targets for kernels 1-9 except K10, and
K11-K12 closely. K14 has an error up to about 1.35e-3 relative. Kernel 10 and
Kernel 13 still fail materially:

| Kernel | Span | Accurate48 observed | Original LFK target |
|---:|---:|---:|---:|
| 10 | 3 | -1.966253912e3 | 1.651291228e3 |
| 13 | 1 | 6.712639175e7 | 4.468741170e8 |
| 13 | 2 | 1.609608794e7 | 2.323352390e8 |
| 13 | 3 | 9.926795581e5 | 4.143805389e7 |
| 14 | 3 | 2.294897616e6 | 2.297991960e6 |

Kernel 2 and Kernel 4 had exact translation/checksum mistakes in the first
attempt and are now matching the reference. The remaining mismatches mean the
current run is still not fidelity-validated; do not use its throughput table
as an official or historical apples-to-apples LFK result. It also runs one
test run rather than the standard seven and does not reproduce LFK clock
calibration. The separate Cray-1 reference band and implementation details are in
`main/asm/PAIR48_DEV_LOG.md`.

The Datamation September 1984 Fig. 3 table and Wolcott's Table 5-5 provide an
independent Cray-1 CFT '84 per-kernel reference: 100.0, 41.7, 33.3, 24.3, 7.7,
7.0, 120.0, 55.4, 68.0, 36.0, 2.9, 25.0, 4.0, 5.6 MFLOPS for kernels 1-14.
Keep it separate from the Cray-1S table pending confirmation of precision and
run conventions.

Follow-up: verified that K10's `[101][25]` and K13's `[512][4]` C layouts
already make their contiguous checksum walks identical to Fortran physical
storage. K13's `[64][64]` H array is accessed transposed (`h[j][i]`), which
also preserves Fortran H(i,j) storage order. The K13 full H and active P
restores match Netlib TEST. Added the documented 30-value seed-256 IQRANF
vector as a runtime check; it prints PASS. Accurate48 integer conversions now
use the complete high-plus-low value, and K14 VX addition follows the
Fortran's left-associative order. These changes built and ran on the board but
did not resolve K10/K13 or the span-dependent K14 checksum error; the latest
numbers and details are logged in PAIR48_DEV_LOG.md.

## Configuration (sdkconfig.defaults)
```ini
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_CLK_IO=30
CONFIG_SPIRAM_CS_IO=26
CONFIG_SPIRAM_SPEED_40M=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_COMPILER_OPTIMIZATION_PERF=y
CONFIG_ESP_TASK_WDT=y
```

## Project files

- `main/livermore_main.c` - application entry point
- `components/livermore/` - active benchmark implementation
- `main/asm/` - Pair48 arithmetic library and development log
- `sdkconfig.defaults` - ESP32-S3 build defaults

## Pre-correction Pair48 elementwise audit (2026-10-01)

The current firmware now audits Pair48 against Accurate48 element by element
across the precise checksum-contributing slices for all 14 kernels and three
span sets. It runs outside the timed measurement. The ESP32-S3 run confirmed
that an identical aggregate checksum can hide numerical differences: K1 span
1 had checksum-relative error 4.268e-16 but 183 numerically different values
(max relative element error 4.354e-15); K9 span 1 had zero checksum difference
with 26 different values; K10 spans 1 and 2 had zero checksum difference
with 221 different values (max relative error 2.620e-9). K4 was exactly equal
in all its audited values. The firmware prints the complete per-kernel/span
table. These results describe the pre-correction Pair48-versus-Accurate48 run only.
Its K10/K13/K14 outputs used incorrect per-iteration restores and are
superseded by the state-repeat correction below.

The final audit build passed and was flashed/run on ESP32-S3 with scalar and
bulk Pair48 self-tests at zero mismatches and the seed-256 IQRANF vector
passing. The source tree now uses `components/livermore/` as its sole Livermore
implementation and `main/asm/` as the canonical Pair48 library location;
stale duplicate trees and old nested source archives were removed. See
`docs/LIVERMORE_PORT.md` and this consolidated development log for details.


## Previous audited board run before state-repeat correction (2026-10-01)

This pre-correction run reported geometric means of 32.773 MFLOPS FP32, 8.100
MFLOPS Pair48, and 4.748 MFLOPS Accurate48. Its K10/K13/K14 checksum failures
were caused by restoring state on every inner iteration; these results are
historical and superseded by the state-repeat correction below.


## LFK alignment correction (2026-10-01)

The prior K10/K13/K14 checksum failures came from restoring state after every
inner kernel iteration. LFK's `Loop` repetitions are stateful; those restores
were incorrectly placed inside the repeated loop. Removed the per-iteration
restores from K10, K13, and K14 (and from the K13 index-path diagnostic). A
native binary64 reference using the same original loops then reproduced the
published targets exactly for those kernels.

The corrected firmware built and ran on the ESP32-S3. Accurate48 now matches
the published K10, K13, and K14 checksum values for all three spans; K10 is
exact at printed precision, while K13/K14 relative errors are at most about
1.0e-14. K1-K14 all agree with published targets to Accurate48 precision;
K12 is the outlier because its checksum is cancellation-sensitive, with
relative errors 1.473e-9, 1.012e-10, and 2.356e-10 for spans 1-3. The complete
42-row checksum and elementwise audit is emitted by firmware; the detailed
Pair48-versus-Accurate48 audit is recorded in `main/asm/PAIR48_DEV_LOG.md`.

Latest geometric means across 42 samples are 43.555 MFLOPS FP32, 9.074
MFLOPS Pair48, and 5.200 MFLOPS Accurate48. These remain provisional as
performance results because the port does not yet run all 24 kernels or the
standard seven timing trials and calibration. Pair48's K13 index path can
diverge from Accurate48 at rounding-sensitive cell boundaries, so its close
aggregate checksum should not be read as identical particle paths.
