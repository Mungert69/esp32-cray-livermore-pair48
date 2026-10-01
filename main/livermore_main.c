/*
 * Minimal main entry point for Livermore benchmark
 * Only compiled when CONFIG_BENCHMARK_TYPE_LIVERMORE is set
 */

#include <stdio.h>
#include "sdkconfig.h"
#include "pair48_asm.h"

#if CONFIG_BENCHMARK_TYPE_LIVERMORE
extern void livermore_app_main(void);

void app_main(void)
{
    printf("ESP32-S3 Livermore Kernels 1-14 Benchmark\n");
    pair48_library_selftest();
    livermore_app_main();
}
#endif
