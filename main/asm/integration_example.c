/* Minimal integration example */

#include "pair48_asm.h"

void example_pair48_use(void)
{
    static pair48_t x[128] __attribute__((aligned(16)));
    static pair48_t y[128] __attribute__((aligned(16)));
    static pair48_t z[128] __attribute__((aligned(16)));

    pair48_t alpha = pair48_from_double(1.0000001193092896);

    pair48_add_array_asm(z, x, y, 128);
    pair48_mul_array_asm(z, x, y, 128);

    pair48_saxpy_asm(y, x, &alpha, 128);

    pair48_t sum = pair48_sum(y, 128);
    pair48_t dot = pair48_dot(x, y, 128);

    (void)sum;
    (void)dot;
}
