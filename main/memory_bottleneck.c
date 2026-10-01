#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "perfmon.h"
#include "xtensa/xt_perf_consts.h"
#include "pair48_asm.h"

#define N            1024
#define REPEATS      256
#define CPU_MHZ      240.0

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
 * Hardware fused operations
 * ================================================================ */

static inline __attribute__((always_inline))
float fma_s(float acc, float a, float b)
{
    __asm__("madd.s %0, %1, %2" : "+f"(acc) : "f"(a), "f"(b));
    return acc;
}

static inline __attribute__((always_inline))
float fms_s(float acc, float a, float b)
{
    __asm__("msub.s %0, %1, %2" : "+f"(acc) : "f"(a), "f"(b));
    return acc;
}


/* ================================================================
 * Double-single arithmetic
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
    return (ds48_t){ .hi = hi, .lo = lo };
}

static inline __attribute__((always_inline))
ds48_t fast_two_sum(float a, float b)
{
    float hi = a + b;
    float z  = hi - a;
    float lo = b - z;
    return (ds48_t){ .hi = hi, .lo = lo };
}

static inline __attribute__((always_inline))
ds48_t ds48_mul(ds48_t x, ds48_t y)
{
    float p = x.hi * y.hi;
    float neg_error = fms_s(p, x.hi, y.hi);
    float t0 = x.lo * y.lo;
    float t1 = fma_s(t0, x.hi, y.lo);
    float t2 = fma_s(t1, x.lo, y.hi);
    float low = t2 - neg_error;
    return fast_two_sum(p, low);
}

static inline __attribute__((always_inline))
ds48_t ds48_add(ds48_t x, ds48_t y)
{
    ds48_t s = two_sum(x.hi, y.hi);
    ds48_t t = two_sum(x.lo, y.lo);
    float c = s.lo + t.hi;
    ds48_t v = fast_two_sum(s.hi, c);
    float w = t.lo + v.lo;
    return fast_two_sum(v.hi, w);
}


static volatile float sink;

/* ================================================================
 * Test 1: Compute-only DS48 (no memory traffic after load)
 * ================================================================ */

static IRAM_ATTR __attribute__((noinline, optimize("O3")))
void ds48_compute_only(ds48_t *restrict y, const ds48_t *restrict x, ds48_t a, int n, int repeats)
{
    for (int r = 0; r < repeats; ++r) {
        ds48_t y_reg[8];
        ds48_t x_reg[8];

        // Load once
        for (int i = 0; i < 8; ++i) {
            x_reg[i] = x[i];
            y_reg[i] = y[i];
        }

        // Pure compute loop
        for (int i = 0; i < 8; ++i) {
            ds48_t p = ds48_mul(a, x_reg[i]);
            y_reg[i] = ds48_add(p, y_reg[i]);
        }

        // Dummy write to prevent optimization
        sink = y_reg[4].hi + y_reg[4].lo;
    }
}


/* ================================================================
 * Test 2: Pure SRAM bandwidth kernels
 * ================================================================ */

static IRAM_ATTR __attribute__((noinline, optimize("O3")))
void memcopy_u64(uint64_t *restrict dst, const uint64_t *restrict src, int n)
{
    for (int i = 0; i < n; ++i) {
        dst[i] = src[i];
    }
}

static IRAM_ATTR __attribute__((noinline, optimize("O3")))
void memtriad_u64(uint64_t *restrict y, const uint64_t *restrict x, const uint64_t *restrict z, int n)
{
    for (int i = 0; i < n; ++i) {
        y[i] = x[i] + z[i];
    }
}


/* ================================================================
 * DS48 unroll-2 kernel (best from previous benchmark)
 * ================================================================ */

static IRAM_ATTR __attribute__((noinline, optimize("O3")))
void saxpy_ds48_u2(
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
        ds48_t p0 = ds48_mul(a, x0);
        ds48_t p1 = ds48_mul(a, x1);
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
 * Data arrays - multiple placements for test 4
 * ================================================================ */

static DRAM_ATTR __attribute__((aligned(16)))
ds48_t x48_shared[N];
static DRAM_ATTR __attribute__((aligned(16)))
ds48_t y48_shared[N];

static DRAM_ATTR __attribute__((aligned(16)))
ds48_t x48_core0[N];
static DRAM_ATTR __attribute__((aligned(16)))
ds48_t y48_core0[N];

static DRAM_ATTR __attribute__((aligned(16)))
ds48_t x48_core1[N];
static DRAM_ATTR __attribute__((aligned(16)))
ds48_t y48_core1[N];

// For SRAM bandwidth tests
static DRAM_ATTR __attribute__((aligned(16)))
uint64_t src_arr[N];
static DRAM_ATTR __attribute__((aligned(16)))
uint64_t dst_arr[N];
static DRAM_ATTR __attribute__((aligned(16)))
uint64_t z_arr[N];


/* ================================================================
 * Data initialization
 * ================================================================ */

static void init_ds48(ds48_t *x, ds48_t *y, int n)
{
    for (int i = 0; i < n; ++i) {
        float bx = 1.0f + (float)(i & 255) * 0.001f;
        float by = 0.5f + (float)(i & 127) * 0.002f;
        x[i].hi = bx; x[i].lo = ((i & 1) ? 1.0e-9f : -1.0e-9f);
        y[i].hi = by; y[i].lo = ((i & 2) ? 8.0e-10f : -8.0e-10f);
    }
}

static void init_u64(uint64_t *x, uint64_t *y, uint64_t *z, int n)
{
    for (int i = 0; i < n; ++i) {
        x[i] = (uint64_t)i * 0x100000001ULL;
        y[i] = (uint64_t)(i + 1000) * 0x100000001ULL;
        z[i] = (uint64_t)(i + 2000) * 0x100000001ULL;
    }
}


static uint32_t g_cycle_start;

static IRAM_ATTR void perf_cycle_start(void) { g_cycle_start = esp_cpu_get_cycle_count(); }


/* ================================================================
 * Performance counter helpers
 * ================================================================ */

typedef struct {
    uint32_t d_stall_busy;
    uint32_t d_stall_bank;
    uint32_t bubbles_reg_dep;
    uint32_t total_cycles;
} perf_data_t;

static IRAM_ATTR void perf_start(void)
{
    // Configure D_STALL counter (counter 0)
    // XTPERF_CNT_D_STALL = select value for data stall
    // XTPERF_MASK_D_STALL_BUSY | XTPERF_MASK_D_STALL_BANK_CONFLICT
    xtensa_perfmon_init(0, XTPERF_CNT_D_STALL,
                        XTPERF_MASK_D_STALL_BUSY | XTPERF_MASK_D_STALL_BANK_CONFLICT,
                        0, -1);

    // Configure BUBBLES counter (counter 1)
    // XTPERF_CNT_BUBBLES = select value for bubbles
    // XTPERF_MASK_BUBBLES_R_HOLD_REG_DEP = register dependency bubbles
    xtensa_perfmon_init(1, XTPERF_CNT_BUBBLES,
                        XTPERF_MASK_BUBBLES_R_HOLD_REG_DEP,
                        0, -1);

    xtensa_perfmon_start();
}

static IRAM_ATTR void perf_read(perf_data_t *out)
{
    xtensa_perfmon_stop();

    uint32_t d_stall = xtensa_perfmon_value(0);
    uint32_t bubbles = xtensa_perfmon_value(1);
    uint32_t cycles = esp_cpu_get_cycle_count() - g_cycle_start;

    out->d_stall_busy = d_stall; // combined stall (BUSY + BANK_CONFLICT)
    out->d_stall_bank = 0;
    out->bubbles_reg_dep = bubbles;
    out->total_cycles = cycles;
}


/* ================================================================
 * Dual-core infrastructure
 * ================================================================ */

typedef struct {
    TaskHandle_t task;
    void (*fn)(void *);
    void *arg;
    EventBits_t done_bit;
} worker_t;

static EventGroupHandle_t done_group;
static worker_t worker0, worker1;

static void worker_entry(void *arg)
{
    worker_t *w = (worker_t *)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        w->fn(w->arg);
        xEventGroupSetBits(done_group, w->done_bit);
    }
}

static void create_workers(void)
{
    done_group = xEventGroupCreate();

    worker0.done_bit = DONE_CORE0;
    worker1.done_bit = DONE_CORE1;

    xTaskCreatePinnedToCore(worker_entry, "test_core0", 4096, &worker0,
                            configMAX_PRIORITIES - 2, &worker0.task, 0);
    xTaskCreatePinnedToCore(worker_entry, "test_core1", 4096, &worker1,
                            configMAX_PRIORITIES - 2, &worker1.task, 1);

    vTaskDelay(pdMS_TO_TICKS(50));
}

static double run_dual(void (*fn0)(void *), void *arg0,
                       void (*fn1)(void *), void *arg1)
{
    xEventGroupClearBits(done_group, DONE_CORE0 | DONE_CORE1);

    worker0.fn = fn0; worker0.arg = arg0;
    worker1.fn = fn1; worker1.arg = arg1;

    int64_t start = esp_timer_get_time();

    // Wake other core first
    BaseType_t core = xPortGetCoreID();
    if (core == 0) {
        xTaskNotifyGive(worker1.task);
        xTaskNotifyGive(worker0.task);
    } else {
        xTaskNotifyGive(worker0.task);
        xTaskNotifyGive(worker1.task);
    }

    xEventGroupWaitBits(done_group, DONE_CORE0 | DONE_CORE1,
                        pdTRUE, pdTRUE, portMAX_DELAY);

    int64_t elapsed_us = esp_timer_get_time() - start;
    return (double)N * REPEATS / (double)elapsed_us;
}


/* ================================================================
 * Test 1: Compute-only DS48
 * ================================================================ */

typedef struct {
    ds48_t *x;
    ds48_t *y;
    ds48_t a;
} compute_args_t;

static void compute_worker(void *arg)
{
    compute_args_t *a = (compute_args_t *)arg;
    ds48_compute_only(a->y, a->x, a->a, 8, REPEATS * 1000);
}

static void test_compute_only(void)
{
    printf("\n=== Test 1: Compute-only DS48 (no memory traffic) ===\n");

    ds48_t a = { .hi = 1.0000001f, .lo = 1.0e-10f };
    init_ds48(x48_shared, y48_shared, N);

    compute_args_t args0 = { x48_shared, y48_shared, a };
    compute_args_t args1 = { x48_shared + 8, y48_shared + 8, a };

    // Single core
    perf_cycle_start();
    perf_start();
    ds48_compute_only(y48_shared, x48_shared, a, 8, REPEATS * 1000);
    uint32_t cycles1 = esp_cpu_get_cycle_count() - g_cycle_start;

    perf_data_t perf1 = {0};
    perf_read(&perf1);

    double mel1 = (8.0 * REPEATS * 1000) / ((double)cycles1 / CPU_MHZ);

    // Dual core
    double mel2 = run_dual(compute_worker, &args0, compute_worker, &args1);

    printf("Single-core: %.3f M elements/s (%.2f cycles/element)\n",
           mel1, (double)cycles1 / (8.0 * REPEATS * 1000));
    printf("Dual-core:   %.3f M elements/s (scaling %.3fx)\n",
           mel2, mel2 / mel1);
    printf("Perf: D_STALL=%" PRIu32 " BUBBLES_REG_DEP=%" PRIu32 " cycles=%" PRIu32 "\n",
           perf1.d_stall_busy, perf1.bubbles_reg_dep, perf1.total_cycles);
}


/* ================================================================
 * Test 2: Pure SRAM bandwidth
 * ================================================================ */

typedef struct {
    uint64_t *dst;
    const uint64_t *src;
    const uint64_t *z;
    int repeats;
} bw_args_t;

static void bw_copy_worker(void *arg) {
    bw_args_t *a = (bw_args_t *)arg;
    for (int r = 0; r < a->repeats; ++r)
        memcopy_u64(a->dst, a->src, N);
}

static void bw_triad_worker(void *arg) {
    bw_args_t *a = (bw_args_t *)arg;
    for (int r = 0; r < a->repeats; ++r)
        memtriad_u64(a->dst, a->src, a->z, N);
}

static void test_sram_bandwidth(void)
{
    printf("\n=== Test 2: Pure SRAM Bandwidth ===\n");

    init_u64(src_arr, dst_arr, z_arr, N);

    bw_args_t args0 = { dst_arr, src_arr, z_arr, REPEATS };
    bw_args_t args1 = { dst_arr + N/2, src_arr + N/2, z_arr + N/2, REPEATS };

    // COPY: single core
    perf_cycle_start();
    perf_start();
    for (int r = 0; r < REPEATS; ++r) memcopy_u64(dst_arr, src_arr, N);
    uint32_t c1 = esp_cpu_get_cycle_count() - g_cycle_start;
    perf_data_t p1; perf_read(&p1);
    double bw1 = (N * 8.0 * REPEATS) / ((double)c1 / CPU_MHZ / 1e6);

    // COPY: dual core
    double bw2 = run_dual(bw_copy_worker, &args0, bw_copy_worker, &args1);

    printf("COPY single:  %.1f MB/s  (%.2f cyc/element)\n",
           bw1, (double)c1 / (N * REPEATS));
    printf("COPY dual:    %.1f MB/s  (scaling %.3fx)\n", bw2, bw2/bw1);
    printf("  Perf: D_STALL=%" PRIu32 " BUBBLES=%" PRIu32 "\n", p1.d_stall_busy, p1.bubbles_reg_dep);

    // TRIAD: single core
    perf_cycle_start();
    perf_start();
    for (int r = 0; r < REPEATS; ++r) memtriad_u64(dst_arr, src_arr, z_arr, N);
    c1 = esp_cpu_get_cycle_count() - g_cycle_start;
    perf_read(&p1);
    bw1 = (N * 24.0 * REPEATS) / ((double)c1 / CPU_MHZ / 1e6);

    // TRIAD: dual core
    bw2 = run_dual(bw_triad_worker, &args0, bw_triad_worker, &args1);

    printf("TRIAD single: %.1f MB/s  (%.2f cyc/element)\n",
           bw1, (double)c1 / (N * REPEATS));
    printf("TRIAD dual:   %.1f MB/s  (scaling %.3fx)\n", bw2, bw2/bw1);
    printf("  Perf: D_STALL=%" PRIu32 " BUBBLES=%" PRIu32 "\n", p1.d_stall_busy, p1.bubbles_reg_dep);
}


/* ================================================================
 * Test 3: DS48 with perf counters
 * ================================================================ */

typedef struct {
    ds48_t *x;
    ds48_t *y;
    ds48_t a;
} ds48_args_t;

static void ds48_worker(void *arg)
{
    ds48_args_t *a = (ds48_args_t *)arg;
    for (int r = 0; r < REPEATS; ++r)
        saxpy_ds48_u2(a->y, a->x, a->a, N/2);
}

static void test_ds48_with_counters(void)
{
    printf("\n=== Test 3: DS48 unroll-2 with Performance Counters ===\n");

    ds48_t a = { .hi = 1.0000001f, .lo = 1.0e-10f };

    // Single core on shared arrays
    init_ds48(x48_shared, y48_shared, N);
    perf_cycle_start();
    perf_start();
    for (int r = 0; r < REPEATS; ++r)
        saxpy_ds48_u2(y48_shared, x48_shared, a, N);
    uint32_t c1 = esp_cpu_get_cycle_count() - g_cycle_start;
    perf_data_t p1; perf_read(&p1);
    double mel1 = CPU_MHZ * N * REPEATS / c1;

    // Dual core on shared arrays
    ds48_args_t args0 = { x48_shared, y48_shared, a };
    ds48_args_t args1 = { x48_shared + N/2, y48_shared + N/2, a };
    double mel2 = run_dual(ds48_worker, &args0, ds48_worker, &args1);

    printf("Shared arrays:\n");
    printf("  1-core: %.3f M/s (%.2f cyc/elem) D_STALL=%" PRIu32 " BUBBLES=%" PRIu32 "\n",
           mel1, (double)c1/(N*REPEATS), p1.d_stall_busy, p1.bubbles_reg_dep);
    printf("  2-core: %.3f M/s (scaling %.3fx)\n", mel2, mel2/mel1);

    // Single core on separate arrays
    init_ds48(x48_core0, y48_core0, N);
    init_ds48(x48_core1, y48_core1, N);
    perf_cycle_start();
    perf_start();
    for (int r = 0; r < REPEATS; ++r)
        saxpy_ds48_u2(y48_core0, x48_core0, a, N);
    c1 = esp_cpu_get_cycle_count() - g_cycle_start;
    perf_read(&p1);
    mel1 = CPU_MHZ * N * REPEATS / c1;

    // Dual core on separate arrays
    args0.x = x48_core0; args0.y = y48_core0; args0.a = a;
    args1.x = x48_core1; args1.y = y48_core1; args1.a = a;
    mel2 = run_dual(ds48_worker, &args0, ds48_worker, &args1);

    printf("Separate arrays:\n");
    printf("  1-core: %.3f M/s (%.2f cyc/elem) D_STALL=%" PRIu32 " BUBBLES=%" PRIu32 "\n",
           mel1, (double)c1/(N*REPEATS), p1.d_stall_busy, p1.bubbles_reg_dep);
    printf("  2-core: %.3f M/s (scaling %.3fx)\n", mel2, mel2/mel1);
}


/* ================================================================
 * Test 4: SRAM placement/alignment
 * ================================================================ */

static void test_placement(void)
{
    printf("\n=== Test 4: SRAM Placement/Alignment Effects ===\n");

    ds48_t a = { .hi = 1.0000001f, .lo = 1.0e-10f };

    // Test 1: shared arrays (baseline)
    printf("Shared arrays (aligned 16, N=%d):\n", N);
    init_ds48(x48_shared, y48_shared, N);
    perf_cycle_start();
    for (int r = 0; r < REPEATS; ++r)
        saxpy_ds48_u2(y48_shared, x48_shared, a, N);
    uint32_t c = esp_cpu_get_cycle_count() - g_cycle_start;
    printf("  1-core: %.3f M/s (%.2f cyc/elem)\n",
           CPU_MHZ * N * REPEATS / c, (double)c/(N*REPEATS));

    // Test 2: separate arrays for each core (no overlap)
    printf("Separate core arrays (N/2 each):\n");
    init_ds48(x48_core0, y48_core0, N/2);
    init_ds48(x48_core1, y48_core1, N/2);

    ds48_args_t args0 = { x48_core0, y48_core0, a };
    ds48_args_t args1 = { x48_core1, y48_core1, a };
    double mel2 = run_dual(ds48_worker, &args0, ds48_worker, &args1);
    printf("  2-core (separate): %.3f M/s\n", mel2);

    // Test 3: shared arrays with 128-byte padding between core regions
    // Use smaller region so we don't overflow
    int half = N/2 - 32;  // leave 32 elements (256 bytes) for padding
    printf("Shared arrays with 128B padding (N=%d each):\n", half);
    init_ds48(x48_shared, y48_shared, half);
    init_ds48(x48_shared + half + 16, y48_shared + half + 16, half);

    ds48_t *x0 = x48_shared;
    ds48_t *y0 = y48_shared;
    ds48_t *x1 = x48_shared + half + 16;
    ds48_t *y1 = y48_shared + half + 16;

    args0.x = x0; args0.y = y0; args0.a = a;
    args1.x = x1; args1.y = y1; args1.a = a;
    mel2 = run_dual(ds48_worker, &args0, ds48_worker, &args1);
    printf("  2-core (128B padding): %.3f M/s\n", mel2);

    // Test 4: shared arrays with 4KB padding
    half = N/2 - 512;  // leave 512 elements (4KB) for padding
    printf("Shared arrays with 4KB padding (N=%d each):\n", half);
    init_ds48(x48_shared, y48_shared, half);
    init_ds48(x48_shared + half + 512, y48_shared + half + 512, half);

    x0 = x48_shared; y0 = y48_shared;
    x1 = x48_shared + half + 512; y1 = y48_shared + half + 512;
    args0.x = x0; args0.y = y0; args0.a = a;
    args1.x = x1; args1.y = y1; args1.a = a;
    mel2 = run_dual(ds48_worker, &args0, ds48_worker, &args1);
    printf("  2-core (4KB padding):  %.3f M/s\n", mel2);
}


/* ================================================================
 * Main
 * ================================================================ */

void app_main(void)
{
    printf("\nESP32-S3 Memory Bottleneck Investigation\n");
    printf("=========================================\n");

    pair48_library_selftest();

    create_workers();

    test_compute_only();
    test_sram_bandwidth();
    test_ds48_with_counters();
    test_placement();

    printf("\n=== Summary ===\n");
    printf("sink=%g\n", sink);
}
