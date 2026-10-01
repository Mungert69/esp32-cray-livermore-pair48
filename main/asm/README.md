# ESP32-S3 Pair48 primitive library

This is a first assembly-library pass for the Pair48 arithmetic used in the
ESP32-S3 vs Cray-1 work.

## Files

- `pair48_asm.h`
  - Pair48 type.
  - Always-inline C reference arithmetic.
  - Public assembly and reduction APIs.

- `pair48_asm.S`
  - C-callable scalar assembly:
    - `pair48_add_asm`
    - `pair48_sub_asm`
    - `pair48_mul_asm`
    - `pair48_mac_asm`
  - Bulk assembly:
    - `pair48_add_array_asm`
    - `pair48_mul_array_asm`
    - `pair48_saxpy_asm`
    - `pair48_dot_asm` (four independent Pair48 accumulators)
    - `pair48_k1_hydro_asm` (whole-loop Livermore Kernel 1 path)

- `pair48_vector.c`
  - Four-accumulator `pair48_sum`
  - Four-accumulator `pair48_dot`
  - These remain C deliberately: the arithmetic inlines, while GCC gets four
    independent chains to schedule.

- `pair48_selftest.c`
  - Compares assembly with the existing Pair48 arithmetic.
  - Checks assembly dot products against `pair48_dot()` for lengths 0–9.
  - Benchmarks assembly SAXPY against an inlined C Pair48 SAXPY.

## Why pointer-based scalar functions?

Xtensa's aggregate argument/return ABI makes a `pair48_t -> pair48_t` assembly
function unnecessarily fragile.  The scalar assembly entry points therefore
use:

    void pair48_mul_asm(pair48_t *out,
                        const pair48_t *x,
                        const pair48_t *y);

This also makes the ABI immediately obvious in disassembly:

    a2 = out
    a3 = x
    a4 = y

The performance-critical paths should normally use the bulk routines, so the
scalar call overhead is not on the hot path.

## Why 64-bit PIE loads first, not 128-bit?

ESP32-S3 `EE.LDF.64.IP` / `EE.STF.64.IP` exactly match one Pair48 value
(two FP32 words), and Espressif uses these forms in its own ESP-DSP float
assembly.  This is a safer first library revision than depending immediately
on the 128-bit FPU-load form.

The bulk multiply and SAXPY kernels still software-unroll two Pair48 elements,
so they have two independent arithmetic chains.  Once this version builds and
is numerically verified on your toolchain, the next revision can test
`EE.LDF.128.IP` / `EE.STF.128.IP` for two Pair48 values in one memory
instruction.

## ESP-IDF integration

Copy the four source/header files into `main/`.

Example component CMake:

    idf_component_register(
        SRCS
            "cray_benchmark.c"
            "pair48_asm.S"
            "pair48_vector.c"
            "pair48_selftest.c"
        INCLUDE_DIRS "."
    )

Then include:

    #include "pair48_asm.h"

and call once from `app_main()` while bringing the library up:

    pair48_library_selftest();

## Using it from C kernels

Simple array kernels can call a whole-vector primitive:

    pair48_add_array_asm(dst, a, b, n);
    pair48_mul_array_asm(dst, a, b, n);
    pair48_saxpy_asm(y, x, &alpha, n);

Reductions:

    pair48_t s = pair48_sum(x, n);
    pair48_t q = pair48_dot(x, y, n);

For an assembly dot product, use the pointer-based entry point:

    pair48_dot_asm(&q, x, y, n);

For complicated Livermore kernels, continue using the inline Pair48 arithmetic
from `pair48_asm.h` first.  That has no per-operation function-call overhead.
Only replace whole loop bodies with assembly when the benchmark shows it is
worth doing.

The optional Kernel 1 hydro routine owns both the pass and element loops. Its
current measured gain is about 1.2% over Pair48 C; Kernel 3 remains on C because
its assembly dot variants did not beat GCC's generated loop.

## Expected first result

Your current Pair48 C SAXPY is about 31.6 cycles/element.  This assembly SAXPY
is intended to test whether explicit two-element scheduling plus 64-bit PIE
loads/stores can move materially below that.

Do not assume a target cycle count before measuring.  The important first
criteria are:

1. zero scalar mismatches;
2. zero bulk mismatches;
3. stable result after repeated SAXPY passes;
4. then cycles/element.

## Important build note

This source targets ESP32-S3/LX7 and uses S3 PIE instructions.  It is not a
portable Xtensa assembly implementation.
