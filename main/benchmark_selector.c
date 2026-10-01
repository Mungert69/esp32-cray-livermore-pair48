/*
 * Benchmark Selector - chooses between Cray SAXPY and Livermore Kernels
 * based on Kconfig configuration.
 */

#include <stdio.h>
#include "sdkconfig.h"
#include "pair48_asm.h"

// Forward declarations
#if CONFIG_BENCHMARK_TYPE_CRAY
void run_cray_benchmark(void);
#elif CONFIG_BENCHMARK_TYPE_LIVERMORE
void livermore_app_main(void);
#endif

void app_main(void)
{
    printf("ESP32-S3 Benchmark Selector\n");

    /* Run Pair48 library self-test first */
    pair48_library_selftest();

#if CONFIG_BENCHMARK_TYPE_CRAY
    printf("Running: Cray SAXPY Benchmark\n");
    run_cray_benchmark();
#elif CONFIG_BENCHMARK_TYPE_LIVERMORE
    printf("Running: Livermore Kernels 1-14 Benchmark\n");
    livermore_app_main();
#else
    printf("No benchmark selected! Set CONFIG_BENCHMARK_TYPE_* in menuconfig.\n");
#endif
}
