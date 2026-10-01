#include "pair48_asm.h"

/*
 * Four independent accumulators are important on the ESP32-S3: they give
 * the compiler enough independent FP chains to hide much of the FPU latency.
 *
 * These reductions deliberately do not promise bit-identical results to a
 * sequential reduction because the association order is different.
 */

__attribute__((noinline, optimize("O3")))
pair48_t pair48_sum(const pair48_t *x, int n)
{
    pair48_t s0 = {0.0f, 0.0f};
    pair48_t s1 = {0.0f, 0.0f};
    pair48_t s2 = {0.0f, 0.0f};
    pair48_t s3 = {0.0f, 0.0f};

    int i = 0;

    for (; i + 3 < n; i += 4) {
        s0 = pair48_add_inline(s0, x[i + 0]);
        s1 = pair48_add_inline(s1, x[i + 1]);
        s2 = pair48_add_inline(s2, x[i + 2]);
        s3 = pair48_add_inline(s3, x[i + 3]);
    }

    pair48_t s = pair48_add_inline(s0, s1);
    s = pair48_add_inline(s, s2);
    s = pair48_add_inline(s, s3);

    for (; i < n; ++i) {
        s = pair48_add_inline(s, x[i]);
    }

    return s;
}


__attribute__((noinline, optimize("O3")))
pair48_t pair48_dot(
    const pair48_t *x,
    const pair48_t *y,
    int n)
{
    pair48_t s0 = {0.0f, 0.0f};
    pair48_t s1 = {0.0f, 0.0f};
    pair48_t s2 = {0.0f, 0.0f};
    pair48_t s3 = {0.0f, 0.0f};

    int i = 0;

    for (; i + 3 < n; i += 4) {
        s0 = pair48_mac_inline(s0, x[i + 0], y[i + 0]);
        s1 = pair48_mac_inline(s1, x[i + 1], y[i + 1]);
        s2 = pair48_mac_inline(s2, x[i + 2], y[i + 2]);
        s3 = pair48_mac_inline(s3, x[i + 3], y[i + 3]);
    }

    pair48_t s = pair48_add_inline(s0, s1);
    s = pair48_add_inline(s, s2);
    s = pair48_add_inline(s, s3);

    for (; i < n; ++i) {
        s = pair48_mac_inline(s, x[i], y[i]);
    }

    return s;
}
