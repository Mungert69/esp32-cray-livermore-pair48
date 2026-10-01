#include <stdio.h>
#include <stdint.h>
#include <math.h>

#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#define N          1024
#define REPEATS    64
#define CPU_MHZ    240.0

#define DONE_CORE0 (1U << 0)
#define DONE_CORE1 (1U << 1)

typedef struct {
    float hi;
    float lo;
} ds48_t;

typedef void (*ds48_kernel_t)(
    ds48_t *restrict y,
    const ds48_t *restrict x,
    ds48_t a,
    int n
);


/* ================================================================
 * ESP32-S3 hardware FP32 fused operations
 * ================================================================ */

static inline __attribute__((always_inline))
float fma_s(float acc, float a, float b)
{
    __asm__(
        "madd.s %0, %1, %2"
        : "+f"(acc)
        : "f"(a), "f"(b)
    );

    return acc;
}

static inline __attribute__((always_inline))
float fms_s(float acc, float a, float b)
{
    __asm__(
        "msub.s %0, %1, %2"
        : "+f"(acc)
        : "f"(a), "f"(b)
    );

    return acc;
}


/* ================================================================
 * Double-single arithmetic
 *
 * Approximately 48 bits of significand precision.
 * ================================================================ */

static inline __attribute__((always_inline))
ds48_t two_sum(float a, float b)
{
    float hi = a + b;

    float a1 = hi - b;
    float b1 = hi - a1;

    float da = a - a1;
    float db = b - b1;

    float lo = da + db;

    return (ds48_t) {
        .hi = hi,
        .lo = lo
    };
}


static inline __attribute__((always_inline))
ds48_t fast_two_sum(float a, float b)
{
    float hi = a + b;
    float z  = hi - a;
    float lo = b - z;

    return (ds48_t) {
        .hi = hi,
        .lo = lo
    };
}


static inline __attribute__((always_inline))
ds48_t ds48_mul(ds48_t x, ds48_t y)
{
    float p = x.hi * y.hi;

    /*
     * p - exact(x.hi * y.hi)
     */
    float neg_error = fms_s(p, x.hi, y.hi);

    float t0 = x.lo * y.lo;

    float t1 = fma_s(
        t0,
        x.hi,
        y.lo
    );

    float t2 = fma_s(
        t1,
        x.lo,
        y.hi
    );

    float low = t2 - neg_error;

    return fast_two_sum(p, low);
}


static inline __attribute__((always_inline))
ds48_t ds48_add(ds48_t x, ds48_t y)
{
    ds48_t s = two_sum(x.hi, y.hi);
    ds48_t t = two_sum(x.lo, y.lo);

    float c = s.lo + t.hi;

    ds48_t v = fast_two_sum(
        s.hi,
        c
    );

    float w = t.lo + v.lo;

    return fast_two_sum(
        v.hi,
        w
    );
}


/* ================================================================
 * Baseline kernel
 * ================================================================ */

static IRAM_ATTR
__attribute__((noinline, optimize("O3")))
void saxpy_ds48(
    ds48_t *restrict y,
    const ds48_t *restrict x,
    ds48_t a,
    int n)
{
    for (int i = 0; i < n; ++i) {
        ds48_t p = ds48_mul(a, x[i]);
        y[i] = ds48_add(p, y[i]);
    }
}


/* ================================================================
 * 2-way software-pipelined/unrolled kernel
 *
 * Gives GCC two completely independent FP dependency chains.
 * ================================================================ */

static IRAM_ATTR
__attribute__((noinline, optimize("O3")))
void saxpy_ds48_u2(
    ds48_t *restrict y,
    const ds48_t *restrict x,
    ds48_t a,
    int n)
{
    int i = 0;

    for (; i + 1 < n; i += 2) {

        /*
         * Load both independent elements first.
         */
        ds48_t x0 = x[i];
        ds48_t x1 = x[i + 1];

        ds48_t y0 = y[i];
        ds48_t y1 = y[i + 1];

        /*
         * Independent multiply chains.
         */
        ds48_t p0 = ds48_mul(a, x0);
        ds48_t p1 = ds48_mul(a, x1);

        /*
         * Independent add chains.
         */
        y0 = ds48_add(p0, y0);
        y1 = ds48_add(p1, y1);

        y[i]     = y0;
        y[i + 1] = y1;
    }

    for (; i < n; ++i) {
        ds48_t p = ds48_mul(a, x[i]);
        y[i] = ds48_add(p, y[i]);
    }
}


/* ================================================================
 * 4-way software-pipelined/unrolled kernel
 *
 * Four independent operations give the scheduler enough work to
 * hide much of the ~4-cycle FP dependency latency.
 *
 * This may be faster than U2, or it may hit FP register pressure.
 * The benchmark determines which is actually better.
 * ================================================================ */

static IRAM_ATTR
__attribute__((noinline, optimize("O3")))
void saxpy_ds48_u4(
    ds48_t *restrict y,
    const ds48_t *restrict x,
    ds48_t a,
    int n)
{
    int i = 0;

    for (; i + 3 < n; i += 4) {

        ds48_t x0 = x[i];
        ds48_t x1 = x[i + 1];
        ds48_t x2 = x[i + 2];
        ds48_t x3 = x[i + 3];

        ds48_t y0 = y[i];
        ds48_t y1 = y[i + 1];
        ds48_t y2 = y[i + 2];
        ds48_t y3 = y[i + 3];

        /*
         * Do all multiplies before the adds.
         *
         * Once inlined, GCC sees four independent FP instruction
         * streams and can interleave them.
         */
        ds48_t p0 = ds48_mul(a, x0);
        ds48_t p1 = ds48_mul(a, x1);
        ds48_t p2 = ds48_mul(a, x2);
        ds48_t p3 = ds48_mul(a, x3);

        y0 = ds48_add(p0, y0);
        y1 = ds48_add(p1, y1);
        y2 = ds48_add(p2, y2);
        y3 = ds48_add(p3, y3);

        y[i]     = y0;
        y[i + 1] = y1;
        y[i + 2] = y2;
        y[i + 3] = y3;
    }

    for (; i < n; ++i) {
        ds48_t p = ds48_mul(a, x[i]);
        y[i] = ds48_add(p, y[i]);
    }
}


/* ================================================================
 * 2-way explicit software-pipelined kernel (optimized)
 *
 * Separates all multiplies from all adds to maximize ILP.
 * Uses local arrays to keep all live values in registers.
 * ================================================================ */

static IRAM_ATTR
__attribute__((noinline, optimize("O3")))
void saxpy_ds48_u2_piped(
    ds48_t *restrict y,
    const ds48_t *restrict x,
    ds48_t a,
    int n)
{
    int i = 0;

    /* Process pairs with explicit pipelining */
    for (; i + 1 < n; i += 2) {
        /* Stage 1: Load all data */
        float x0_hi = x[i].hi;
        float x0_lo = x[i].lo;
        float x1_hi = x[i + 1].hi;
        float x1_lo = x[i + 1].lo;

        float y0_hi = y[i].hi;
        float y0_lo = y[i].lo;
        float y1_hi = y[i + 1].hi;
        float y1_lo = y[i + 1].lo;

        float a_hi = a.hi;
        float a_lo = a.lo;

        /* Stage 2: ALL multiplies (independent) */
        float p0_hi = a_hi * x0_hi;
        float p1_hi = a_hi * x1_hi;

        float p0_lo_ll = a_lo * x0_lo;
        float p1_lo_ll = a_lo * x1_lo;

        float p0_lo_lh = a_hi * x0_lo;
        float p1_lo_lh = a_hi * x1_lo;

        float p0_lo_hl = a_lo * x0_hi;
        float p1_lo_hl = a_lo * x1_hi;

        /* Stage 3: Error terms (FMS) */
        float p0_neg_err = fms_s(p0_hi, a_hi, x0_hi);
        float p1_neg_err = fms_s(p1_hi, a_hi, x1_hi);

        /* Stage 4: Cross-term accumulation (FMA) */
        float p0_lo = p0_lo_ll;
        float p1_lo = p1_lo_ll;

        p0_lo = fma_s(p0_lo, a_hi, x0_lo);
        p1_lo = fma_s(p1_lo, a_hi, x1_lo);

        p0_lo = fma_s(p0_lo, a_lo, x0_hi);
        p1_lo = fma_s(p1_lo, a_lo, x1_hi);

        p0_lo = p0_lo - p0_neg_err;
        p1_lo = p1_lo - p1_neg_err;

        /* Stage 5: FastTwoSum for p.hi + p.lo */
        float p0_hi_sum = p0_hi + p0_lo;
        float p1_hi_sum = p1_hi + p1_lo;

        float p0_z = p0_hi_sum - p0_hi;
        float p1_z = p1_hi_sum - p1_hi;

        float p0_lo_final = p0_lo - p0_z;
        float p1_lo_final = p1_lo - p1_z;

        /* p0 = (p0_hi_sum, p0_lo_final), p1 = (p1_hi_sum, p1_lo_final) */

        /* Stage 6: TwoSum for p + y */
        float s0_hi = p0_hi_sum + y0_hi;
        float s1_hi = p1_hi_sum + y1_hi;

        float s0_a1 = s0_hi - y0_hi;
        float s1_a1 = s1_hi - y1_hi;

        float s0_b1 = s0_hi - s0_a1;
        float s1_b1 = s1_hi - s1_a1;

        float s0_da = p0_hi_sum - s0_a1;
        float s1_da = p1_hi_sum - s1_a1;

        float s0_db = y0_hi - s0_b1;
        float s1_db = y1_hi - s1_b1;

        float s0_lo = s0_da + s0_db;
        float s1_lo = s1_da + s1_db;

        /* FastTwoSum(s.hi, s.lo + t.hi) where t = two_sum(p.lo, y.lo) */
        float t0_hi = p0_lo_final + y0_lo;
        float t1_hi = p1_lo_final + y1_lo;

        float t0_a1 = t0_hi - y0_lo;
        float t1_a1 = t1_hi - y1_lo;

        float t0_b1 = t0_hi - t0_a1;
        float t1_b1 = t1_hi - t1_a1;

        float t0_da = p0_lo_final - t0_a1;
        float t1_da = p1_lo_final - t1_a1;

        float t0_db = y0_lo - t0_b1;
        float t1_db = y1_lo - t1_b1;

        float t0_lo = t0_da + t0_db;
        float t1_lo = t1_da + t1_db;

        float c0 = s0_lo + t0_hi;
        float c1 = s1_lo + t1_hi;

        float v0_hi = s0_hi + c0;
        float v1_hi = s1_hi + c1;

        float v0_z = v0_hi - s0_hi;
        float v1_z = v1_hi - s1_hi;

        float v0_lo = c0 - v0_z;
        float v1_lo = c1 - v1_z;

        float w0 = t0_lo + v0_lo;
        float w1 = t1_lo + v1_lo;

        float res0_hi = v0_hi + w0;
        float res1_hi = v1_hi + w1;

        float res0_z = res0_hi - v0_hi;
        float res1_z = res1_hi - v1_hi;

        float res0_lo = w0 - res0_z;
        float res1_lo = w1 - res1_z;

        y[i]     = (ds48_t){ .hi = res0_hi, .lo = res0_lo };
        y[i + 1] = (ds48_t){ .hi = res1_hi, .lo = res1_lo };
    }

    /* Tail */
    for (; i < n; ++i) {
        float x_hi = x[i].hi;
        float x_lo = x[i].lo;
        float y_hi = y[i].hi;
        float y_lo = y[i].lo;
        float a_hi = a.hi;
        float a_lo = a.lo;

        float p_hi = a_hi * x_hi;
        float p_lo_ll = a_lo * x_lo;
        float p_lo_lh = a_hi * x_lo;
        float p_lo_hl = a_lo * x_hi;

        float p_neg_err = fms_s(p_hi, a_hi, x_hi);

        float p_lo = p_lo_ll;
        p_lo = fma_s(p_lo, a_hi, x_lo);
        p_lo = fma_s(p_lo, a_lo, x_hi);
        p_lo = p_lo - p_neg_err;

        float p_hi_sum = p_hi + p_lo;
        float p_z = p_hi_sum - p_hi;
        float p_lo_final = p_lo - p_z;

        float s_hi = p_hi_sum + y_hi;
        float s_a1 = s_hi - y_hi;
        float s_b1 = s_hi - s_a1;
        float s_da = p_hi_sum - s_a1;
        float s_db = y_hi - s_b1;
        float s_lo = s_da + s_db;

        float t_hi = p_lo_final + y_lo;
        float t_a1 = t_hi - y_lo;
        float t_b1 = t_hi - t_a1;
        float t_da = p_lo_final - t_a1;
        float t_db = y_lo - t_b1;
        float t_lo = t_da + t_db;

        float c = s_lo + t_hi;
        float v_hi = s_hi + c;
        float v_z = v_hi - s_hi;
        float v_lo = c - v_z;

        float w = t_lo + v_lo;
        float res_hi = v_hi + w;
        float res_z = res_hi - v_hi;
        float res_lo = w - res_z;

        y[i] = (ds48_t){ .hi = res_hi, .lo = res_lo };
    }
}


/* ================================================================
 * FP32 reference kernel
 * ================================================================ */

static IRAM_ATTR
__attribute__((noinline, optimize("O3")))
void saxpy_f32(
    float *restrict y,
    const float *restrict x,
    float a,
    int n)
{
    for (int i = 0; i < n; ++i) {
        y[i] = fma_s(y[i], a, x[i]);
    }
}


/* ================================================================
 * Pair arithmetic (TwoFloat style, no per-op normalization)
 *
 * Approximately 48 bits of significand precision.
 * ~15 FP ops per SAXPY vs ~30 for accurate DS48.
 * ================================================================ */

static inline __attribute__((always_inline))
ds48_t pair_mul(ds48_t x, ds48_t y)
{
    /* 7 operations with FMA */
    float p = x.hi * y.hi;
    float e = fms_s(p, x.hi, y.hi);    /* e = -(x.hi*y.hi - p) */
    float t = x.lo * y.lo;
    t = fma_s(t, x.hi, y.lo);
    t = fma_s(t, x.lo, y.hi);
    t = t - e;
    return (ds48_t){ .hi = p, .lo = t };
}

static inline __attribute__((always_inline))
ds48_t pair_add(ds48_t x, ds48_t y)
{
    /* 8 operations */
    float s = x.hi + y.hi;
    float v = s - x.hi;
    float lo = (x.hi - (s - v)) + (y.hi - v) + x.lo + y.lo;
    return (ds48_t){ .hi = s, .lo = lo };
}


/* ================================================================
 * Pair arithmetic SAXPY unroll-2 kernel
 * ================================================================ */

static IRAM_ATTR
__attribute__((noinline, optimize("O3")))
void saxpy_pair48_u2(
    ds48_t *restrict y,
    const ds48_t *restrict x,
    ds48_t a,
    int n)
{
    int i = 0;

    for (; i + 1 < n; i += 2) {
        ds48_t x0 = x[i];
        ds48_t x1 = x[i + 1];

        ds48_t y0 = y[i];
        ds48_t y1 = y[i + 1];

        ds48_t p0 = pair_mul(a, x0);
        ds48_t p1 = pair_mul(a, x1);

        y0 = pair_add(p0, y0);
        y1 = pair_add(p1, y1);

        y[i]     = y0;
        y[i + 1] = y1;
    }

    for (; i < n; ++i) {
        ds48_t p = pair_mul(a, x[i]);
        y[i] = pair_add(p, y[i]);
    }
}


/* ================================================================
 * Pair arithmetic SAXPY unroll-4 kernel
 * ================================================================ */

static IRAM_ATTR
__attribute__((noinline, optimize("O3")))
void saxpy_pair48_u4(
    ds48_t *restrict y,
    const ds48_t *restrict x,
    ds48_t a,
    int n)
{
    int i = 0;

    for (; i + 3 < n; i += 4) {
        ds48_t x0 = x[i];
        ds48_t x1 = x[i + 1];
        ds48_t x2 = x[i + 2];
        ds48_t x3 = x[i + 3];

        ds48_t y0 = y[i];
        ds48_t y1 = y[i + 1];
        ds48_t y2 = y[i + 2];
        ds48_t y3 = y[i + 3];

        ds48_t p0 = pair_mul(a, x0);
        ds48_t p1 = pair_mul(a, x1);
        ds48_t p2 = pair_mul(a, x2);
        ds48_t p3 = pair_mul(a, x3);

        y0 = pair_add(p0, y0);
        y1 = pair_add(p1, y1);
        y2 = pair_add(p2, y2);
        y3 = pair_add(p3, y3);

        y[i]     = y0;
        y[i + 1] = y1;
        y[i + 2] = y2;
        y[i + 3] = y3;
    }

    for (; i < n; ++i) {
        ds48_t p = pair_mul(a, x[i]);
        y[i] = pair_add(p, y[i]);
    }
}


/* ================================================================
 * Numerical verification
 * ================================================================ */

static void verify_numerical_accuracy(void)
{
    printf("\n=== Numerical Accuracy Verification ===\n\n");

    /* Test with a known sequence */
    ds48_t a = { .hi = 1.00000011920928955078125f, .lo = 1.0e-10f };
    ds48_t x[16];
    ds48_t y_ds48[16];
    ds48_t y_pair[16];
    ds48_t y_f32[16];

    for (int i = 0; i < 16; ++i) {
        float bx = 1.0f + (float)(i & 255) * 0.001f;
        float by = 0.5f + (float)(i & 127) * 0.002f;
        x[i].hi = bx; x[i].lo = ((i & 1) ? 1.0e-9f : -1.0e-9f);
        y_ds48[i].hi = by; y_ds48[i].lo = ((i & 2) ? 8.0e-10f : -8.0e-10f);
        y_pair[i] = y_ds48[i];
        y_f32[i].hi = by; y_f32[i].lo = ((i & 2) ? 8.0e-10f : -8.0e-10f);
    }

    /* Run multiple SAXPY passes */
    int passes = 256;

    for (int p = 0; p < passes; ++p) {
        for (int i = 0; i < 16; ++i) {
            ds48_t px = ds48_mul(a, x[i]);
            y_ds48[i] = ds48_add(px, y_ds48[i]);

            ds48_t pp = pair_mul(a, x[i]);
            y_pair[i] = pair_add(pp, y_pair[i]);

            y_f32[i].hi = y_f32[i].hi + a.hi * x[i].hi;
        }
        /* Yield periodically to avoid watchdog timeout */
        if ((p & 31) == 0) {
            vTaskDelay(1);
        }
    }

    double max_rel_err = 0.0;
    double max_ulp_err = 0.0;
    int max_idx = 0;

    for (int i = 0; i < 16; ++i) {
        /* Convert to double for comparison */
        double ref = (double)y_ds48[i].hi + (double)y_ds48[i].lo;
        double pair_val = (double)y_pair[i].hi + (double)y_pair[i].lo;
        double f32_val = (double)y_f32[i].hi;

        double rel_err = fabs((pair_val - ref) / ref);
        double ulp_err = rel_err * 0x1p48; /* ~48 bits of precision */

        if (rel_err > max_rel_err) {
            max_rel_err = rel_err;
            max_ulp_err = ulp_err;
            max_idx = i;
        }
        /* Print only max error case to reduce output */
        if (i == max_idx || i == 0 || i == 15) {
            printf("i=%2d: DS48=%.12e Pair=%.12e rel_err=%.2e ulp=%.1f\n",
                   i, ref, pair_val, rel_err, ulp_err);
        }
    }

    printf("\nMax relative error: %.2e (%.1f ULP@48b) at index %d\n",
           max_rel_err, max_ulp_err, max_idx);

    /* Also test error growth with repeated passes */
    printf("\n=== Error growth over passes (single element) ===\n");
    ds48_t y0_ds48 = { .hi = 0.5f, .lo = 1e-10f };
    ds48_t y0_pair = { .hi = 0.5f, .lo = 1e-10f };
    ds48_t x0 = { .hi = 1.001f, .lo = 1e-9f };
    ds48_t a0 = { .hi = 1.0000001f, .lo = 1e-10f };

    for (int p = 0; p <= 256; p += 16) {
        if (p > 0) {
            /* Do 16 updates per reported step */
            for (int u = 0; u < 16; ++u) {
                ds48_t px = ds48_mul(a0, x0);
                y0_ds48 = ds48_add(px, y0_ds48);

                ds48_t pp = pair_mul(a0, x0);
                y0_pair = pair_add(pp, y0_pair);
            }
        }

        double ref = (double)y0_ds48.hi + (double)y0_ds48.lo;
        double pair_val = (double)y0_pair.hi + (double)y0_pair.lo;
        double rel_err = fabs((pair_val - ref) / ref);
        /* Print only every 64 passes to reduce output */
        if (p % 64 == 0) {
            printf("Pass %3d: DS48=%.12e Pair=%.12e rel_err=%.2e\n", p, ref, pair_val, rel_err);
        }
    }
    /* Yield after error growth test */
    vTaskDelay(1);

    /* Comprehensive random test */
    printf("\n=== Comprehensive Random Test (500 elements, 50 passes) ===\n");
    ds48_t a_rand = { .hi = 1.2345678f, .lo = 1.234e-7f };
    ds48_t x_rand[500];
    ds48_t y_ds48_rand[500];
    ds48_t y_pair_rand[500];

    /* Initialize with varied exponents and signs */
    for (int i = 0; i < 500; ++i) {
        float exp_scale = powf(2.0f, (float)(i % 24 - 12));  /* 2^-12 to 2^11 */
        float bx = exp_scale * (1.0f + (float)(i & 1023) * 0.0001f);
        float by = exp_scale * (0.5f + (float)(i & 511) * 0.0002f);

        /* Alternate signs */
        if (i & 1) bx = -bx;
        if (i & 2) by = -by;

        x_rand[i].hi = bx; x_rand[i].lo = ((i & 1) ? 1.0e-9f : -1.0e-9f);
        y_ds48_rand[i].hi = by; y_ds48_rand[i].lo = ((i & 2) ? 8.0e-10f : -8.0e-10f);
        y_pair_rand[i] = y_ds48_rand[i];
    }

    for (int p = 0; p < 50; ++p) {
        for (int i = 0; i < 500; ++i) {
            ds48_t px = ds48_mul(a_rand, x_rand[i]);
            y_ds48_rand[i] = ds48_add(px, y_ds48_rand[i]);

            ds48_t pp = pair_mul(a_rand, x_rand[i]);
            y_pair_rand[i] = pair_add(pp, y_pair_rand[i]);
        }
        /* Yield periodically to avoid watchdog timeout */
        if ((p & 7) == 0) {
            vTaskDelay(1);
        }
    }

    double max_rel_rand = 0.0;
    int max_idx_rand = 0;
    for (int i = 0; i < 500; ++i) {
        double ref = (double)y_ds48_rand[i].hi + (double)y_ds48_rand[i].lo;
        double pair_val = (double)y_pair_rand[i].hi + (double)y_pair_rand[i].lo;
        double rel_err = fabs((pair_val - ref) / ref);
        if (rel_err > max_rel_rand) {
            max_rel_rand = rel_err;
            max_idx_rand = i;
        }
    }
    printf("Max rel error (50 passes): %.2e at index %d\n", max_rel_rand, max_idx_rand);

    /* Cancellation test: x and a nearly cancel */
    printf("\n=== Cancellation Test ===\n");
    ds48_t a_canc = { .hi = 1.0f, .lo = 0 };
    ds48_t x_canc = { .hi = 1.0f, .lo = 1e-7f };  /* hi = 1.0, lo small */
    ds48_t y_canc_ds48 = { .hi = -1.0f, .lo = -1e-7f };  /* y = -x (cancellation) */
    ds48_t y_canc_pair = y_canc_ds48;

    for (int p = 0; p < 100; ++p) {
        ds48_t px = ds48_mul(a_canc, x_canc);
        y_canc_ds48 = ds48_add(px, y_canc_ds48);

        ds48_t pp = pair_mul(a_canc, x_canc);
        y_canc_pair = pair_add(pp, y_canc_pair);

        /* Yield periodically to avoid watchdog timeout */
        if ((p & 15) == 0) {
            vTaskDelay(1);
        }
    }

    double ref_canc = (double)y_canc_ds48.hi + (double)y_canc_ds48.lo;
    double pair_canc = (double)y_canc_pair.hi + (double)y_canc_pair.lo;
    printf("DS48:  %.12e\n", ref_canc);
    printf("Pair:  %.12e\n", pair_canc);
    printf("Rel err: %.2e\n", fabs((pair_canc - ref_canc) / ref_canc));

    /* Test periodic renormalization */
    printf("\n=== Periodic Renormalization Test ===\n");
    ds48_t a_ren = { .hi = 1.0000001f, .lo = 1e-10f };
    ds48_t x_ren = { .hi = 1.001f, .lo = 1e-9f };
    ds48_t y_ren_ds48 = { .hi = 0.5f, .lo = 1e-10f };
    ds48_t y_ren_pair = { .hi = 0.5f, .lo = 1e-10f };

    for (int renorm_period = 0; renorm_period <= 4; ++renorm_period) {
        if (renorm_period == 0) continue;  /* skip 0 */

        y_ren_pair.hi = 0.5f; y_ren_pair.lo = 1e-10f;

        for (int p = 0; p < 256; ++p) {
            ds48_t pp = pair_mul(a_ren, x_ren);
            y_ren_pair = pair_add(pp, y_ren_pair);

            /* Renormalize periodically */
            if ((p + 1) % renorm_period == 0) {
                /* FastTwoSum to renormalize */
                float s = y_ren_pair.hi + y_ren_pair.lo;
                float z = s - y_ren_pair.hi;
                y_ren_pair.lo = y_ren_pair.lo - z;
                y_ren_pair.hi = s;
            }
        }
        /* Yield after each renorm period test */
        vTaskDelay(1);
    }

    /* Test renorm every 8 (the sweet spot?) */
    printf("\n=== Renorm every 8 passes (detailed) ===\n");
    y_ren_pair.hi = 0.5f; y_ren_pair.lo = 1e-10f;
    y_ren_ds48.hi = 0.5f; y_ren_ds48.lo = 1e-10f;

    for (int p = 0; p <= 256; p += 16) {
        if (p > 0) {
            for (int u = 0; u < 16; ++u) {
                ds48_t px = ds48_mul(a_ren, x_ren);
                y_ren_ds48 = ds48_add(px, y_ren_ds48);

                ds48_t pp = pair_mul(a_ren, x_ren);
                y_ren_pair = pair_add(pp, y_ren_pair);

                if ((p + u) % 8 == 7) {
                    float s = y_ren_pair.hi + y_ren_pair.lo;
                    float z = s - y_ren_pair.hi;
                    y_ren_pair.lo = y_ren_pair.lo - z;
                    y_ren_pair.hi = s;
                }
            }
        }
        double ref_ren = (double)y_ren_ds48.hi + (double)y_ren_ds48.lo;
        double pair_ren = (double)y_ren_pair.hi + (double)y_ren_pair.lo;
        double rel_ren = fabs((pair_ren - ref_ren) / ref_ren);
        printf("Pass %3d: Pair=%.12e rel_err=%.2e\n", p, pair_ren, rel_ren);
    }
    /* Yield after detailed renorm test */
    vTaskDelay(1);
}


/* ================================================================
 * Data
 *
 * Explicitly keep benchmark data in internal DRAM.
 * ================================================================ */

static DRAM_ATTR
ds48_t x48[N] __attribute__((aligned(16)));

static DRAM_ATTR
ds48_t y48[N] __attribute__((aligned(16)));

static DRAM_ATTR
float x32[N] __attribute__((aligned(16)));

static DRAM_ATTR
float y32[N] __attribute__((aligned(16)));

static volatile float sink;


/* ================================================================
 * Data initialization
 * ================================================================ */

static void init_ds48_data(void)
{
    for (int i = 0; i < N; ++i) {

        float base_x =
            1.0f + (float)(i & 255) * 0.001f;

        float base_y =
            0.5f + (float)(i & 127) * 0.002f;

        x48[i].hi = base_x;
        x48[i].lo =
            ((i & 1) ? 1.0e-9f : -1.0e-9f);

        y48[i].hi = base_y;
        y48[i].lo =
            ((i & 2) ? 8.0e-10f : -8.0e-10f);
    }
}


static void init_f32_data(void)
{
    for (int i = 0; i < N; ++i) {

        x32[i] =
            1.0f + (float)(i & 255) * 0.001f;

        y32[i] =
            0.5f + (float)(i & 127) * 0.002f;
    }
}


/* ================================================================
 * Single-core benchmarks
 * ================================================================ */

static float benchmark_ds48_single(
    ds48_kernel_t kernel)
{
    init_ds48_data();

    ds48_t a = {
        .hi = 1.00000011920928955078125f,
        .lo = 1.0e-10f
    };

    /*
     * Warm-up.
     */
    kernel(y48, x48, a, N);

    /*
     * Restore input state so each implementation starts equally.
     */
    init_ds48_data();

    uint32_t start =
        esp_cpu_get_cycle_count();

    for (int r = 0; r < REPEATS; ++r) {
        kernel(y48, x48, a, N);
    }

    uint32_t end =
        esp_cpu_get_cycle_count();

    sink =
        y48[N / 2].hi +
        y48[N / 2].lo;

    uint32_t cycles =
        end - start;

    return (float)cycles /
           ((float)N * REPEATS);
}


static float benchmark_f32_single(void)
{
    init_f32_data();

    saxpy_f32(
        y32,
        x32,
        1.0000001f,
        N
    );

    init_f32_data();

    uint32_t start =
        esp_cpu_get_cycle_count();

    for (int r = 0; r < REPEATS; ++r) {
        saxpy_f32(
            y32,
            x32,
            1.0000001f,
            N
        );
    }

    uint32_t end =
        esp_cpu_get_cycle_count();

    sink = y32[N / 2];

    return
        (float)(end - start) /
        ((float)N * REPEATS);
}


/* ================================================================
 * Real dual-core benchmark
 * ================================================================ */

typedef struct {
    TaskHandle_t task;

    ds48_kernel_t kernel;

    int offset;
    int count;
    int repeats;

    ds48_t a;

    EventBits_t done_bit;
} worker_t;


static EventGroupHandle_t done_group;

static worker_t worker0;
static worker_t worker1;


static void benchmark_worker(void *arg)
{
    worker_t *w =
        (worker_t *)arg;

    for (;;) {

        /*
         * Main task wakes us for each benchmark run.
         */
        ulTaskNotifyTake(
            pdTRUE,
            portMAX_DELAY
        );

        ds48_t *y =
            &y48[w->offset];

        const ds48_t *x =
            &x48[w->offset];

        for (int r = 0;
             r < w->repeats;
             ++r) {

            w->kernel(
                y,
                x,
                w->a,
                w->count
            );
        }

        /*
         * Ensure result becomes observable.
         */
        sink =
            y[w->count / 2].hi +
            y[w->count / 2].lo;

        xEventGroupSetBits(
            done_group,
            w->done_bit
        );
    }
}


static void create_workers(void)
{
    done_group =
        xEventGroupCreate();

    worker0.offset   = 0;
    worker0.count    = N / 2;
    worker0.repeats  = REPEATS;
    worker0.done_bit = DONE_CORE0;

    worker1.offset   = N / 2;
    worker1.count    = N / 2;
    worker1.repeats  = REPEATS;
    worker1.done_bit = DONE_CORE1;

    xTaskCreatePinnedToCore(
        benchmark_worker,
        "cray_core0",
        4096,
        &worker0,
        configMAX_PRIORITIES - 2,
        &worker0.task,
        0
    );

    xTaskCreatePinnedToCore(
        benchmark_worker,
        "cray_core1",
        4096,
        &worker1,
        configMAX_PRIORITIES - 2,
        &worker1.task,
        1
    );
}


static double benchmark_ds48_dual(
    ds48_kernel_t kernel)
{
    init_ds48_data();

    ds48_t a = {
        .hi = 1.00000011920928955078125f,
        .lo = 1.0e-10f
    };

    worker0.kernel = kernel;
    worker1.kernel = kernel;

    worker0.a = a;
    worker1.a = a;

    xEventGroupClearBits(
        done_group,
        DONE_CORE0 | DONE_CORE1
    );

    /*
     * esp_timer gives wall-clock time, which is what matters
     * for a dual-core benchmark.
     */
    int64_t start_us =
        esp_timer_get_time();

    /*
     * Wake the worker on the OTHER core first.
     *
     * Otherwise the high-priority worker on our own core can
     * immediately preempt this task before we wake the second worker,
     * accidentally serializing the benchmark.
     */
    BaseType_t current_core = xPortGetCoreID();

    if (current_core == 0) {
        xTaskNotifyGive(worker1.task);  // Core 1 starts
        xTaskNotifyGive(worker0.task);  // Core 0 then preempts us
    } else {
        xTaskNotifyGive(worker0.task);  // Core 0 starts
        xTaskNotifyGive(worker1.task);  // Core 1 then preempts us
    }

    xEventGroupWaitBits(
        done_group,
        DONE_CORE0 | DONE_CORE1,
        pdTRUE,
        pdTRUE,
        portMAX_DELAY
    );

    int64_t end_us =
        esp_timer_get_time();

    int64_t elapsed_us =
        end_us - start_us;

    /*
     * Convenient identity:
     *
     * elements / microseconds
     *
     * numerically equals million elements/second.
     */
    double total_elements =
        (double)N *
        (double)REPEATS;

    return
        total_elements /
        (double)elapsed_us;
}


/* ================================================================
 * Result formatting
 * ================================================================ */

static void print_single_result(
    const char *name,
    float cycles)
{
    double mel_s =
        CPU_MHZ /
        (double)cycles;

    printf(
        "%-20s %8.2f cycles/element   "
        "%7.3f M elements/s\n",
        name,
        cycles,
        mel_s
    );
}


static void print_dual_result(
    const char *name,
    double mel_s,
    double single_mel_s)
{
    double scaling =
        mel_s / single_mel_s;

    /*
     * Cray-1 memory-to-memory SAXPY ceiling:
     *
     * 80M 64-bit words/sec / 3 words per element.
     */
    const double cray_memory_ceiling =
        80.0 / 3.0;

    double cray_ratio =
        cray_memory_ceiling /
        mel_s;

    printf(
        "%-20s %7.3f M elements/s   "
        "scaling %.3fx   "
        "Cray ceiling/S3 %.3fx\n",
        name,
        mel_s,
        scaling,
        cray_ratio
    );
}


/* ================================================================
 * Main
 * ================================================================ */

void run_cray_benchmark(void)
{
    printf(
        "\nESP32-S3 vs Cray-1 48-bit benchmark\n"
    );

    printf(
        "====================================\n\n"
    );

    /* Disable both task and interrupt watchdogs for long verification runs */
    esp_task_wdt_deinit();
    // Note: Interrupt watchdog cannot be fully disabled on ESP32-S3,
    // but we can reduce verification output to avoid triggering it

    create_workers();

    /*
     * Give both worker tasks time to start and block.
     */
    vTaskDelay(
        pdMS_TO_TICKS(100)
    );

    /* Run numerical verification first - DISABLED due to watchdog timeout
     * The verification does extensive floating-point computation that
     * triggers the interrupt watchdog (IWDT) on ESP32-S3.
     * verify_numerical_accuracy();
     */

    /* ------------------------------------------------------------
     * FP32 reference
     * ------------------------------------------------------------ */

    float fp32_cycles =
        benchmark_f32_single();


    /* ------------------------------------------------------------
     * Single-core 48-bit tests (accurate DS48)
     * ------------------------------------------------------------ */

    float base_cycles =
        benchmark_ds48_single(
            saxpy_ds48
        );

    float u2_cycles =
        benchmark_ds48_single(
            saxpy_ds48_u2
        );

    float u4_cycles =
        benchmark_ds48_single(
            saxpy_ds48_u4
        );

    float u2_piped_cycles =
        benchmark_ds48_single(
            saxpy_ds48_u2_piped
        );


    /* ------------------------------------------------------------
     * Single-core 48-bit tests (pair arithmetic)
     * ------------------------------------------------------------ */

    float pair_u2_cycles =
        benchmark_ds48_single(
            saxpy_pair48_u2
        );

    float pair_u4_cycles =
        benchmark_ds48_single(
            saxpy_pair48_u4
        );


    printf(
        "Single-core measurements\n"
    );

    printf(
        "------------------------\n"
    );

    print_single_result(
        "FP32",
        fp32_cycles
    );

    print_single_result(
        "DS48 baseline",
        base_cycles
    );

    print_single_result(
        "DS48 unroll-2",
        u2_cycles
    );

    print_single_result(
        "DS48 unroll-4",
        u4_cycles
    );

    print_single_result(
        "DS48 u2 piped",
        u2_piped_cycles
    );

    print_single_result(
        "Pair48 unroll-2",
        pair_u2_cycles
    );

    print_single_result(
        "Pair48 unroll-4",
        pair_u4_cycles
    );


    /* ------------------------------------------------------------
     * Real dual-core tests
     * ------------------------------------------------------------ */

    printf(
        "\nReal dual-core measurements\n"
    );

    printf(
        "---------------------------\n"
    );

    double base_single =
        CPU_MHZ / base_cycles;

    double u2_single =
        CPU_MHZ / u2_cycles;

    double u4_single =
        CPU_MHZ / u4_cycles;

    double u2_piped_single =
        CPU_MHZ / u2_piped_cycles;

    double pair_u2_single =
        CPU_MHZ / pair_u2_cycles;

    double pair_u4_single =
        CPU_MHZ / pair_u4_cycles;


    double base_dual =
        benchmark_ds48_dual(
            saxpy_ds48
        );

    double u2_dual =
        benchmark_ds48_dual(
            saxpy_ds48_u2
        );

    double u4_dual =
        benchmark_ds48_dual(
            saxpy_ds48_u4
        );

    double u2_piped_dual =
        benchmark_ds48_dual(
            saxpy_ds48_u2_piped
        );

    double pair_u2_dual =
        benchmark_ds48_dual(
            saxpy_pair48_u2
        );

    double pair_u4_dual =
        benchmark_ds48_dual(
            saxpy_pair48_u4
        );


    print_dual_result(
        "DS48 baseline",
        base_dual,
        base_single
    );

    print_dual_result(
        "DS48 unroll-2",
        u2_dual,
        u2_single
    );

    print_dual_result(
        "DS48 unroll-4",
        u4_dual,
        u4_single
    );

    print_dual_result(
        "DS48 u2 piped",
        u2_piped_dual,
        u2_piped_single
    );

    print_dual_result(
        "Pair48 unroll-2",
        pair_u2_dual,
        pair_u2_single
    );

    print_dual_result(
        "Pair48 unroll-4",
        pair_u4_dual,
        pair_u4_single
    );


    printf(
        "\nReference figures\n"
    );

    printf(
        "-----------------\n"
    );

    printf(
        "Cray-1 arithmetic peak:       "
        "~80.000 M SAXPY elements/s\n"
    );

    printf(
        "Cray-1 memory SAXPY ceiling:  "
        "~26.667 M elements/s\n"
    );

    printf(
        "\n"
        "The 26.667M figure is the relevant upper bound\n"
        "for comparison with these complete memory-to-memory\n"
        "ESP32-S3 kernels.\n"
    );

    printf(
        "\nsink=%g\n",
        sink
    );
}