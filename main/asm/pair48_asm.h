#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float hi;
    float lo;
} pair48_t;

/*
 * ESP32-S3 fused FP32 helpers.
 *
 * MADD.S/MSUB.S are used explicitly so the Pair48 multiply residual
 * has the intended single-rounding behaviour.
 */
static inline __attribute__((always_inline))
float pair48_fma_s(float acc, float a, float b)
{
#if defined(__XTENSA__)
    __asm__ volatile (
        "madd.s %0, %1, %2"
        : "+f"(acc)
        : "f"(a), "f"(b)
    );
    return acc;
#else
    return __builtin_fmaf(a, b, acc);
#endif
}

static inline __attribute__((always_inline))
float pair48_fms_s(float acc, float a, float b)
{
#if defined(__XTENSA__)
    __asm__ volatile (
        "msub.s %0, %1, %2"
        : "+f"(acc)
        : "f"(a), "f"(b)
    );
    return acc;
#else
    return __builtin_fmaf(-a, b, acc);
#endif
}

/*
 * Reference/inline Pair48 arithmetic.
 *
 * These intentionally match the Pair48 arithmetic used by the
 * existing ESP32 Cray benchmark.  They do not renormalize after
 * every operation.
 */
static inline __attribute__((always_inline))
pair48_t pair48_add_inline(pair48_t x, pair48_t y)
{
    float s = x.hi + y.hi;
    float v = s - x.hi;

    float lo =
        (x.hi - (s - v))
        + (y.hi - v)
        + x.lo
        + y.lo;

    return (pair48_t){ .hi = s, .lo = lo };
}

static inline __attribute__((always_inline))
pair48_t pair48_sub_inline(pair48_t x, pair48_t y)
{
    y.hi = -y.hi;
    y.lo = -y.lo;
    return pair48_add_inline(x, y);
}

static inline __attribute__((always_inline))
pair48_t pair48_mul_inline(pair48_t x, pair48_t y)
{
    float p = x.hi * y.hi;

    /* e = rounded_product - exact_product */
    float e = pair48_fms_s(p, x.hi, y.hi);

    float t = x.lo * y.lo;
    t = pair48_fma_s(t, x.hi, y.lo);
    t = pair48_fma_s(t, x.lo, y.hi);
    t = t - e;

    return (pair48_t){ .hi = p, .lo = t };
}

static inline __attribute__((always_inline))
pair48_t pair48_mac_inline(pair48_t acc, pair48_t x, pair48_t y)
{
    return pair48_add_inline(acc, pair48_mul_inline(x, y));
}

static inline __attribute__((always_inline))
double pair48_to_double(pair48_t x)
{
    return (double)x.hi + (double)x.lo;
}

static inline __attribute__((always_inline))
pair48_t pair48_from_double(double x)
{
    float hi = (float)x;
    float lo = (float)(x - (double)hi);
    return (pair48_t){ .hi = hi, .lo = lo };
}


/*
 * Scalar assembly primitives.
 *
 * Pointer-based API deliberately avoids Xtensa struct-return / aggregate
 * calling-convention complications.
 */
void pair48_add_asm(
    pair48_t *out,
    const pair48_t *x,
    const pair48_t *y);

void pair48_sub_asm(
    pair48_t *out,
    const pair48_t *x,
    const pair48_t *y);

void pair48_mul_asm(
    pair48_t *out,
    const pair48_t *x,
    const pair48_t *y);

void pair48_mac_asm(
    pair48_t *out,
    const pair48_t *acc,
    const pair48_t *x,
    const pair48_t *y);


/*
 * Bulk assembly primitives.
 *
 * n is a count of Pair48 elements.
 * Arrays should preferably be at least 8-byte aligned.  16-byte alignment
 * is recommended for future 128-bit load/store variants.
 */
void pair48_add_array_asm(
    pair48_t *dst,
    const pair48_t *x,
    const pair48_t *y,
    int n);

void pair48_mul_array_asm(
    pair48_t *dst,
    const pair48_t *x,
    const pair48_t *y,
    int n);

void pair48_saxpy_asm(
    pair48_t *y,
    const pair48_t *x,
    const pair48_t *a,
    int n);

/* Four-accumulator Pair48 dot product, with the complete reduction in ASM. */
void pair48_dot_asm(
    pair48_t *out,
    const pair48_t *x,
    const pair48_t *y,
    int n);

/* Repeat a complete dot product internally, writing the last pass to out. */
void pair48_dot_repeat_asm(
    pair48_t *out,
    const pair48_t *x,
    const pair48_t *y,
    int n,
    int passes);

/* Whole-loop Pair48 Livermore Kernel 1 hydro fragment. */
void pair48_k1_hydro_asm(
    pair48_t *x,
    const pair48_t *y,
    const pair48_t *z10,
    int n,
    int passes);


/*
 * Reduction primitives.
 *
 * pair48_sum() and pair48_dot() are four-way unrolled C using the inline
 * Pair48 operations. pair48_dot_asm() is the assembly alternative for dot
 * products; it uses four independent accumulator chains as well.
 */
pair48_t pair48_sum(
    const pair48_t *x,
    int n);

pair48_t pair48_dot(
    const pair48_t *x,
    const pair48_t *y,
    int n);


/* Optional self-test / microbenchmark. */
void pair48_library_selftest(void);

#ifdef __cplusplus
}
#endif
