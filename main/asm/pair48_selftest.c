#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

#include "esp_cpu.h"

#include "pair48_asm.h"

#define PAIR48_TEST_N 256
#define PAIR48_TEST_REPEATS 512
#define CPU_MHZ 240.0

static pair48_t xbuf[PAIR48_TEST_N] __attribute__((aligned(16)));
static pair48_t ybuf[PAIR48_TEST_N] __attribute__((aligned(16)));
static pair48_t refbuf[PAIR48_TEST_N] __attribute__((aligned(16)));
static pair48_t asmbuf[PAIR48_TEST_N] __attribute__((aligned(16)));

static volatile double pair48_sink;

static void init_data(void)
{
    for (int i = 0; i < PAIR48_TEST_N; ++i) {
        double x = 0.75 + (double)((i * 37) % 211) * 0.00103;
        double y = 0.45 + (double)((i * 53) % 193) * 0.00091;

        xbuf[i] = pair48_from_double(x);
        ybuf[i] = pair48_from_double(y);
    }
}

static double relerr(pair48_t a, pair48_t b)
{
    double da = pair48_to_double(a);
    double db = pair48_to_double(b);
    return fabs(da - db) / (fabs(db) + 1e-300);
}

static int check_scalar(void)
{
    int failures = 0;

    for (int i = 0; i < 32; ++i) {
        pair48_t got, ref;

        ref = pair48_add_inline(xbuf[i], ybuf[i]);
        pair48_add_asm(&got, &xbuf[i], &ybuf[i]);
        if (relerr(got, ref) > 1e-13) failures++;

        ref = pair48_sub_inline(xbuf[i], ybuf[i]);
        pair48_sub_asm(&got, &xbuf[i], &ybuf[i]);
        if (relerr(got, ref) > 1e-13) failures++;

        ref = pair48_mul_inline(xbuf[i], ybuf[i]);
        pair48_mul_asm(&got, &xbuf[i], &ybuf[i]);
        if (relerr(got, ref) > 1e-13) failures++;

        pair48_t acc = pair48_from_double(0.25 + i * 0.0001);
        ref = pair48_mac_inline(acc, xbuf[i], ybuf[i]);
        pair48_mac_asm(&got, &acc, &xbuf[i], &ybuf[i]);
        if (relerr(got, ref) > 1e-13) failures++;
    }

    return failures;
}

static int check_bulk(void)
{
    int failures = 0;

    for (int i = 0; i < PAIR48_TEST_N; ++i)
        refbuf[i] = pair48_add_inline(xbuf[i], ybuf[i]);

    pair48_add_array_asm(asmbuf, xbuf, ybuf, PAIR48_TEST_N);

    for (int i = 0; i < PAIR48_TEST_N; ++i)
        if (relerr(asmbuf[i], refbuf[i]) > 1e-13) failures++;

    for (int i = 0; i < PAIR48_TEST_N; ++i)
        refbuf[i] = pair48_mul_inline(xbuf[i], ybuf[i]);

    pair48_mul_array_asm(asmbuf, xbuf, ybuf, PAIR48_TEST_N);

    for (int i = 0; i < PAIR48_TEST_N; ++i)
        if (relerr(asmbuf[i], refbuf[i]) > 1e-13) failures++;

    pair48_t a = pair48_from_double(1.0000001193092896);

    memcpy(refbuf, ybuf, sizeof(refbuf));
    memcpy(asmbuf, ybuf, sizeof(asmbuf));

    for (int i = 0; i < PAIR48_TEST_N; ++i)
        refbuf[i] = pair48_add_inline(
            refbuf[i],
            pair48_mul_inline(a, xbuf[i]));

    pair48_saxpy_asm(asmbuf, xbuf, &a, PAIR48_TEST_N);

    for (int i = 0; i < PAIR48_TEST_N; ++i)
        if (relerr(asmbuf[i], refbuf[i]) > 1e-13) failures++;

    for (int n = 0; n <= 9; ++n) {
        pair48_t dot_asm;
        pair48_dot_asm(&dot_asm, xbuf, ybuf, n);
        if (relerr(dot_asm, pair48_dot(xbuf, ybuf, n)) > 1e-13)
            failures++;
        pair48_dot_repeat_asm(&dot_asm, xbuf, ybuf, n, 3);
        if (relerr(dot_asm, pair48_dot(xbuf, ybuf, n)) > 1e-13)
            failures++;
    }

    return failures;
}

static float bench_saxpy_asm(void)
{
    pair48_t a = pair48_from_double(1.0000001193092896);
    memcpy(asmbuf, ybuf, sizeof(asmbuf));

    uint32_t start = esp_cpu_get_cycle_count();

    for (int r = 0; r < PAIR48_TEST_REPEATS; ++r)
        pair48_saxpy_asm(asmbuf, xbuf, &a, PAIR48_TEST_N);

    uint32_t end = esp_cpu_get_cycle_count();

    pair48_sink = pair48_to_double(asmbuf[17]);

    return (float)(end - start) /
           ((float)PAIR48_TEST_REPEATS * PAIR48_TEST_N);
}

static float bench_saxpy_c(void)
{
    pair48_t a = pair48_from_double(1.0000001193092896);
    memcpy(refbuf, ybuf, sizeof(refbuf));

    uint32_t start = esp_cpu_get_cycle_count();

    for (int r = 0; r < PAIR48_TEST_REPEATS; ++r) {
        for (int i = 0; i < PAIR48_TEST_N; ++i)
            refbuf[i] = pair48_mac_inline(refbuf[i], a, xbuf[i]);
    }

    uint32_t end = esp_cpu_get_cycle_count();

    pair48_sink = pair48_to_double(refbuf[17]);

    return (float)(end - start) /
           ((float)PAIR48_TEST_REPEATS * PAIR48_TEST_N);
}

void pair48_library_selftest(void)
{
    init_data();

    int scalar_fail = check_scalar();
    int bulk_fail = check_bulk();

    pair48_t s = pair48_sum(xbuf, PAIR48_TEST_N);
    pair48_t d = pair48_dot(xbuf, ybuf, PAIR48_TEST_N);

    float c_cycles = bench_saxpy_c();
    float asm_cycles = bench_saxpy_asm();

    printf("\nPair48 library self-test\n");
    printf("------------------------\n");
    printf("scalar mismatches: %d\n", scalar_fail);
    printf("bulk mismatches:   %d\n", bulk_fail);
    printf("sum:               %.15e\n", pair48_to_double(s));
    printf("dot:               %.15e\n", pair48_to_double(d));
    printf("C SAXPY:           %.2f cycles/element\n", c_cycles);
    printf("ASM SAXPY:         %.2f cycles/element\n", asm_cycles);
    printf("ASM speedup:       %.3fx\n", c_cycles / asm_cycles);
    printf("ASM throughput:    %.3f M elements/s/core\n",
           CPU_MHZ / asm_cycles);
    printf("sink:              %.15e\n", pair48_sink);
}
