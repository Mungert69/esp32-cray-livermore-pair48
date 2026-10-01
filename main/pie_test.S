/* PIE 128-bit load/store test in standalone assembly */

#include <xtensa/coreasm.h>

.section .text, "ax"
.global test_pie_load_store
.type test_pie_load_store, @function

test_pie_load_store:
    /* a2 = src pointer, a3 = dst pointer */
    /* Load first 4 floats (src[0..3]) into f0-f3 */
    ee.ldf.128.ip f0, f1, f2, f3, a2, 16
    /* Load next 4 floats (src[4..7]) into f4-f7 */
    ee.ldf.128.ip f4, f5, f6, f7, a2, 16
    /* Store f0-f3 to dst[0..3] */
    ee.stf.128.ip f0, f1, f2, f3, a3, 16
    /* Store f4-f7 to dst[4..7] */
    ee.stf.128.ip f4, f5, f6, f7, a3, 16
    ret

.size test_pie_load_store, .-test_pie_load_store