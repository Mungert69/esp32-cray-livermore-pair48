#include <stdio.h>
#include <stdint.h>
#include "esp_attr.h"

extern void test_pie_load_store(float *src, float *dst);

static IRAM_ATTR
void test_pie_load_store_c(void)
{
    __attribute__((aligned(16)))
    float src[8] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};

    __attribute__((aligned(16)))
    float dst[8] = {0};

    test_pie_load_store(src, dst);

    printf("Loaded: %.1f %.1f %.1f %.1f\n", src[0], src[1], src[2], src[3]);
    printf("Loaded: %.1f %.1f %.1f %.1f\n", src[4], src[5], src[6], src[7]);
    printf("Stored: %.1f %.1f %.1f %.1f\n", dst[0], dst[1], dst[2], dst[3]);
    printf("Stored: %.1f %.1f %.1f %.1f\n", dst[4], dst[5], dst[6], dst[7]);
}

void app_main(void)
{
    printf("Testing PIE 128-bit load/store...\n");
    test_pie_load_store_c();
    printf("Done.\n");
}