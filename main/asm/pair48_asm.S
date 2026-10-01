/*
 * Pair48 primitives for ESP32-S3 / Xtensa LX7.
 *
 * Arithmetic matches pair48_add_inline() / pair48_mul_inline().
 *
 * Design choices:
 *   - Windowed ABI: ENTRY / RETW.N.
 *   - Pointer-based scalar API: avoids aggregate ABI ambiguity.
 *   - Bulk loops use ESP32-S3 EE.LDF.64.IP / EE.STF.64.IP so one
 *     instruction transfers a complete Pair48 value (hi + lo).
 *   - Bulk multiply and SAXPY process two independent elements together
 *     to hide FP pipeline latency.
 *
 * Verified instruction forms are the same forms used by Espressif ESP-DSP:
 *   ee.ldf.64.ip f1, f0, address_reg, increment
 *       -> first float enters f0, second float enters f1
 *   ee.stf.64.ip f1, f0, address_reg, increment
 *       -> first float stored from f0, second from f1
 */

#include <xtensa/coreasm.h>

.section .iram1, "ax"


/* ========================================================================
 * Scalar add
 * ======================================================================== */

.align 4
.global pair48_add_asm
.type pair48_add_asm, @function
pair48_add_asm:
    entry   a1, 16

    /* x: f0=hi f1=lo, y: f2=hi f3=lo */
    lsi     f0, a3, 0
    lsi     f1, a3, 4
    lsi     f2, a4, 0
    lsi     f3, a4, 4

    /* Exact Pair48 add grouping:
     * lo = (((x.hi-(s-v)) + (y.hi-v)) + x.lo) + y.lo
     */
    add.s   f4, f0, f2         /* s */
    sub.s   f5, f4, f0         /* v */
    sub.s   f6, f4, f5         /* s-v */
    sub.s   f6, f0, f6         /* x.hi-(s-v) */
    sub.s   f5, f2, f5         /* y.hi-v */
    add.s   f6, f6, f5
    add.s   f6, f6, f1
    add.s   f6, f6, f3

    ssi     f4, a2, 0
    ssi     f6, a2, 4

    retw.n
.size pair48_add_asm, .-pair48_add_asm


/* ========================================================================
 * Scalar subtract
 * ======================================================================== */

.align 4
.global pair48_sub_asm
.type pair48_sub_asm, @function
pair48_sub_asm:
    entry   a1, 16

    lsi     f0, a3, 0
    lsi     f1, a3, 4
    lsi     f2, a4, 0
    lsi     f3, a4, 4

    neg.s   f2, f2
    neg.s   f3, f3

    add.s   f4, f0, f2         /* s */
    sub.s   f5, f4, f0         /* v */
    sub.s   f6, f4, f5         /* s-v */
    sub.s   f6, f0, f6
    sub.s   f5, f2, f5
    add.s   f6, f6, f5
    add.s   f6, f6, f1
    add.s   f6, f6, f3

    ssi     f4, a2, 0
    ssi     f6, a2, 4

    retw.n
.size pair48_sub_asm, .-pair48_sub_asm


/* ========================================================================
 * Scalar multiply
 * ======================================================================== */

.align 4
.global pair48_mul_asm
.type pair48_mul_asm, @function
pair48_mul_asm:
    entry   a1, 16

    lsi     f0, a3, 0          /* x.hi */
    lsi     f1, a3, 4          /* x.lo */
    lsi     f2, a4, 0          /* y.hi */
    lsi     f3, a4, 4          /* y.lo */

    mul.s   f4, f0, f2         /* p */
    mul.s   f6, f1, f3         /* low*low */

    mov.s   f5, f4
    msub.s  f5, f0, f2         /* e = p - exact(x.hi*y.hi) */

    madd.s  f6, f0, f3
    madd.s  f6, f1, f2
    sub.s   f6, f6, f5

    ssi     f4, a2, 0
    ssi     f6, a2, 4

    retw.n
.size pair48_mul_asm, .-pair48_mul_asm


/* ========================================================================
 * Scalar MAC: out = acc + x*y
 * ======================================================================== */

.align 4
.global pair48_mac_asm
.type pair48_mac_asm, @function
pair48_mac_asm:
    entry   a1, 16

    /* acc */
    lsi     f0, a3, 0
    lsi     f1, a3, 4

    /* x */
    lsi     f2, a4, 0
    lsi     f3, a4, 4

    /* y */
    lsi     f4, a5, 0
    lsi     f5, a5, 4

    /* Pair multiply -> p.hi=f6, p.lo=f8 */
    mul.s   f6, f2, f4
    mul.s   f8, f3, f5

    mov.s   f7, f6
    msub.s  f7, f2, f4

    madd.s  f8, f2, f5
    madd.s  f8, f3, f4
    sub.s   f8, f8, f7

    /*
     * Pair add: p + acc, preserving the C Pair48 grouping.
     * f6=p.hi f8=p.lo, f0=acc.hi f1=acc.lo
     */
    add.s   f9,  f6, f0        /* s */
    sub.s   f10, f9, f6        /* v */
    sub.s   f11, f9, f10       /* s-v */
    sub.s   f11, f6, f11       /* p.hi-(s-v) */
    sub.s   f10, f0, f10       /* acc.hi-v */
    add.s   f11, f11, f10
    add.s   f11, f11, f8       /* + p.lo */
    add.s   f11, f11, f1       /* + acc.lo */

    ssi     f9,  a2, 0
    ssi     f11, a2, 4

    retw.n
.size pair48_mac_asm, .-pair48_mac_asm


/* ========================================================================
 * Bulk Pair48 add
 *
 * Two elements per software-unrolled iteration.
 * ======================================================================== */

.align 4
.global pair48_add_array_asm
.type pair48_add_array_asm, @function
pair48_add_array_asm:
    entry   a1, 16

    srli    a6, a5, 1          /* pair count */
    movi.n  a7, 1
    and     a7, a5, a7         /* tail = n & 1 */

    bnez    a6, .Ladd_have_pairs
    j       .Ladd_tail
    .align 4
.Ladd_have_pairs:
    loopnez a6, .Ladd_pair_end

    /* x0/x1 */
    ee.ldf.64.ip f1, f0, a3, 8
    ee.ldf.64.ip f3, f2, a3, 8

    /* y0/y1 */
    ee.ldf.64.ip f5, f4, a4, 8
    ee.ldf.64.ip f7, f6, a4, 8

    /* Pair add x+y for two elements, preserving Pair48 grouping. */
    add.s   f8,  f0, f4        /* s0 */
    add.s   f12, f2, f6        /* s1 */

    sub.s   f9,  f8,  f0       /* v0 */
    sub.s   f13, f12, f2       /* v1 */

    sub.s   f10, f8,  f9       /* s0-v0 */
    sub.s   f14, f12, f13      /* s1-v1 */

    sub.s   f10, f0, f10       /* x0.hi error */
    sub.s   f14, f2, f14       /* x1.hi error */

    sub.s   f9,  f4, f9        /* y0.hi error */
    sub.s   f13, f6, f13       /* y1.hi error */

    add.s   f10, f10, f9
    add.s   f14, f14, f13

    add.s   f10, f10, f1       /* + x0.lo */
    add.s   f14, f14, f3       /* + x1.lo */

    add.s   f10, f10, f5       /* + y0.lo */
    add.s   f14, f14, f7       /* + y1.lo */

    ee.stf.64.ip f10, f8,  a2, 8
    ee.stf.64.ip f14, f12, a2, 8

.Ladd_pair_end:

.Ladd_tail:
    beqz    a7, .Ladd_done

    /* one remaining element */
    ee.ldf.64.ip f1, f0, a3, 8
    ee.ldf.64.ip f3, f2, a4, 8

    add.s   f4, f0, f2
    sub.s   f5, f4, f0
    sub.s   f6, f4, f5
    sub.s   f6, f0, f6
    sub.s   f5, f2, f5
    add.s   f6, f6, f5
    add.s   f6, f6, f1
    add.s   f6, f6, f3

    ee.stf.64.ip f6, f4, a2, 8

.Ladd_done:
    retw.n
.size pair48_add_array_asm, .-pair48_add_array_asm


/* ========================================================================
 * Bulk Pair48 multiply
 *
 * Two independent products are deliberately interleaved.  The first four
 * MUL.S instructions create enough independent work that the residual and
 * cross-term instructions can start with much less dependency delay.
 * ======================================================================== */

.align 4
.global pair48_mul_array_asm
.type pair48_mul_array_asm, @function
pair48_mul_array_asm:
    entry   a1, 16

    srli    a6, a5, 1
    movi.n  a7, 1
    and     a7, a5, a7         /* tail = n & 1 */

    bnez    a6, .Lmul_have_pairs
    j       .Lmul_tail
    .align 4
.Lmul_have_pairs:
    loopnez a6, .Lmul_pair_end

    /* x0/x1 */
    ee.ldf.64.ip f1, f0, a3, 8
    ee.ldf.64.ip f3, f2, a3, 8

    /* y0/y1 */
    ee.ldf.64.ip f5, f4, a4, 8
    ee.ldf.64.ip f7, f6, a4, 8

    /* Four independent multiplies. */
    mul.s   f8,  f0, f4        /* p0 */
    mul.s   f9,  f2, f6        /* p1 */
    mul.s   f10, f1, f5        /* t0 */
    mul.s   f11, f3, f7        /* t1 */

    /* Residual accumulators. */
    mov.s   f12, f8
    mov.s   f13, f9
    msub.s  f12, f0, f4
    msub.s  f13, f2, f6

    /* Cross terms. */
    madd.s  f10, f0, f5
    madd.s  f11, f2, f7
    madd.s  f10, f1, f4
    madd.s  f11, f3, f6

    sub.s   f10, f10, f12
    sub.s   f11, f11, f13

    ee.stf.64.ip f10, f8, a2, 8
    ee.stf.64.ip f11, f9, a2, 8

.Lmul_pair_end:

.Lmul_tail:
    beqz    a7, .Lmul_done

    ee.ldf.64.ip f1, f0, a3, 8
    ee.ldf.64.ip f3, f2, a4, 8

    mul.s   f4, f0, f2
    mul.s   f6, f1, f3
    mov.s   f5, f4
    msub.s  f5, f0, f2
    madd.s  f6, f0, f3
    madd.s  f6, f1, f2
    sub.s   f6, f6, f5

    ee.stf.64.ip f6, f4, a2, 8

.Lmul_done:
    retw.n
.size pair48_mul_array_asm, .-pair48_mul_array_asm


/* ========================================================================
 * SAXPY: y[i] = a*x[i] + y[i]
 *
 * Two Pair48 elements are processed together.
 *
 * a2 = y
 * a3 = x
 * a4 = &a
 * a5 = n
 * ======================================================================== */

.align 4
.global pair48_saxpy_asm
.type pair48_saxpy_asm, @function
pair48_saxpy_asm:
    entry   a1, 16

    /* f14=a.hi, f15=a.lo */
    ee.ldf.64.ip f15, f14, a4, 0

    /* separate y read pointer from y write pointer */
    mov.n   a8, a2

    srli    a6, a5, 1
    movi.n  a7, 1
    and     a7, a5, a7         /* tail = n & 1 */

    bnez    a6, .Lsax_have_pairs
    j       .Lsax_tail
    .align 4
.Lsax_have_pairs:
    loopnez a6, .Lsax_pair_end

    /* x0/x1 */
    ee.ldf.64.ip f1, f0, a3, 8
    ee.ldf.64.ip f3, f2, a3, 8

    /* y0/y1 */
    ee.ldf.64.ip f5, f4, a8, 8
    ee.ldf.64.ip f7, f6, a8, 8

    /*
     * Pair multiply a*x.
     * p0.hi=f8, p1.hi=f9
     * p0.lo=f10, p1.lo=f11
     */
    mul.s   f8,  f14, f0
    mul.s   f9,  f14, f2
    mul.s   f10, f15, f1
    mul.s   f11, f15, f3

    mov.s   f12, f8
    mov.s   f13, f9
    msub.s  f12, f14, f0
    msub.s  f13, f14, f2

    madd.s  f10, f14, f1
    madd.s  f11, f14, f3
    madd.s  f10, f15, f0
    madd.s  f11, f15, f2

    sub.s   f10, f10, f12
    sub.s   f11, f11, f13

    /*
     * Pair add p + y, preserving the C Pair48 grouping.
     * Reuse f0-f7 now that x has been consumed.
     */
    add.s   f0, f8, f4         /* s0 */
    add.s   f1, f9, f6         /* s1 */

    sub.s   f12, f0, f8        /* v0 */
    sub.s   f13, f1, f9        /* v1 */

    sub.s   f2, f0, f12        /* s0-v0 */
    sub.s   f3, f1, f13        /* s1-v1 */

    sub.s   f2, f8, f2         /* p0.hi error */
    sub.s   f3, f9, f3         /* p1.hi error */

    sub.s   f12, f4, f12       /* y0.hi error */
    sub.s   f13, f6, f13       /* y1.hi error */

    add.s   f2, f2, f12
    add.s   f3, f3, f13

    add.s   f2, f2, f10        /* + p0.lo */
    add.s   f3, f3, f11        /* + p1.lo */

    add.s   f2, f2, f5         /* + y0.lo */
    add.s   f3, f3, f7         /* + y1.lo */

    ee.stf.64.ip f2, f0, a2, 8
    ee.stf.64.ip f3, f1, a2, 8

.Lsax_pair_end:

.Lsax_tail:
    beqz    a7, .Lsax_done

    /* x */
    ee.ldf.64.ip f1, f0, a3, 8
    /* y */
    ee.ldf.64.ip f3, f2, a8, 8

    /* p = a*x */
    mul.s   f4, f14, f0
    mul.s   f6, f15, f1
    mov.s   f5, f4
    msub.s  f5, f14, f0
    madd.s  f6, f14, f1
    madd.s  f6, f15, f0
    sub.s   f6, f6, f5

    /* p + y, preserving Pair48 grouping */
    add.s   f7,  f4, f2
    sub.s   f8,  f7, f4
    sub.s   f9,  f7, f8
    sub.s   f9,  f4, f9
    sub.s   f8,  f2, f8
    add.s   f9,  f9, f8
    add.s   f9,  f9, f6
    add.s   f9,  f9, f3

    ee.stf.64.ip f9, f7, a2, 8

.Lsax_done:
    retw.n
.size pair48_saxpy_asm, .-pair48_saxpy_asm


/* ========================================================================
 * Four-accumulator Pair48 dot product
 *
 * The four independent Pair48 accumulators hide much of the FPU dependency
 * latency. The final combination and scalar tail follow pair48_dot() order.
 * ======================================================================== */

.macro PAIR48_DOT_MAC acc_hi, acc_lo
    ee.ldf.64.ip f9, f8, a3, 8     /* x.hi, x.lo */
    ee.ldf.64.ip f11, f10, a4, 8  /* y.hi, y.lo */

    mul.s   f12, f8, f10           /* p */
    mul.s   f14, f9, f11           /* lo*lo */
    add.s   f15, \acc_hi, f12      /* start accumulator high merge early */
    mov.s   f13, f12
    msub.s  f13, f8, f10           /* rounded p - exact p */
    madd.s  f14, f8, f11
    madd.s  f14, f9, f10
    sub.s   f14, f14, f13

    /* pair48_add_inline(acc, product), retaining its exact grouping */
    sub.s   f9, f15, \acc_hi       /* v */
    sub.s   f8, f15, f9
    sub.s   f8, \acc_hi, f8
    sub.s   f9, f12, f9
    add.s   f8, f8, f9
    add.s   f8, f8, f14
    add.s   f8, f8, \acc_lo
    mov.s   \acc_hi, f15
    mov.s   \acc_lo, f8
.endm

.macro PAIR48_DOT_ADD_S0 hi, lo
    add.s   f12, f0, \hi
    sub.s   f13, f12, f0
    sub.s   f14, f12, f13
    sub.s   f14, f0, f14
    sub.s   f13, \hi, f13
    add.s   f14, f14, f13
    add.s   f14, f14, f1
    add.s   f14, f14, \lo
    mov.s   f0, f12
    mov.s   f1, f14
.endm

.align 4
.global pair48_dot_asm
.type pair48_dot_asm, @function
pair48_dot_asm:
    entry   a1, 16

    movi.n  a6, 1
    j       .Ldot_repeat_setup
.size pair48_dot_asm, .-pair48_dot_asm

.align 4
.global pair48_dot_repeat_asm
.type pair48_dot_repeat_asm, @function
pair48_dot_repeat_asm:
    entry   a1, 16

.Ldot_repeat_setup:
    mov.n   a8, a3              /* restart x */
    mov.n   a9, a4              /* restart y */
    mov.n   a10, a5             /* n */
    mov.n   a11, a6             /* passes */
    bltz    a10, .Ldot_zero
    beqz    a10, .Ldot_zero
    bltz    a11, .Ldot_zero
    beqz    a11, .Ldot_zero

.Ldot_pass:
    mov.n   a3, a8
    mov.n   a4, a9
    mov.n   a5, a10

    movi.n  a12, 0
    wfr     f0, a12
    wfr     f1, a12
    wfr     f2, a12
    wfr     f3, a12
    wfr     f4, a12
    wfr     f5, a12
    wfr     f6, a12
    wfr     f7, a12

    srli    a6, a5, 2          /* four-element groups */
    movi.n  a7, 3
    and     a7, a5, a7         /* tail = n & 3 */

    bnez    a6, .Ldot_have_groups
    j       .Ldot_tail
    .align 4
.Ldot_have_groups:
    loopnez a6, .Ldot_group_end
    PAIR48_DOT_MAC f0, f1
    PAIR48_DOT_MAC f2, f3
    PAIR48_DOT_MAC f4, f5
    PAIR48_DOT_MAC f6, f7
.Ldot_group_end:

.Ldot_tail:
    PAIR48_DOT_ADD_S0 f2, f3
    PAIR48_DOT_ADD_S0 f4, f5
    PAIR48_DOT_ADD_S0 f6, f7

    beqz    a7, .Ldot_store
    PAIR48_DOT_MAC f0, f1
    addi    a7, a7, -1
    beqz    a7, .Ldot_store
    PAIR48_DOT_MAC f0, f1
    addi    a7, a7, -1
    beqz    a7, .Ldot_store
    PAIR48_DOT_MAC f0, f1

.Ldot_store:
    ssi     f0, a2, 0
    ssi     f1, a2, 4
    addi    a11, a11, -1
    bnez    a11, .Ldot_pass
    retw.n

.Ldot_zero:
    movi.n  a12, 0
    s32i    a12, a2, 0
    s32i    a12, a2, 4
    retw.n
.size pair48_dot_repeat_asm, .-pair48_dot_repeat_asm

/* ========================================================================
 * Livermore Kernel 1 hydro fragment:
 * x[k] = 0.5 + y[k] * (0.125*z[k+10] + 0.25*z[k+11])
 * ======================================================================== */
.macro PAIR48_K1_CALC
    /* Pair48 add: (R*z0) + (T*z1), retaining reference grouping. */
    add.s   f0, f8, f10
    sub.s   f1, f0, f8
    sub.s   f2, f0, f1
    sub.s   f3, f8, f2
    sub.s   f2, f10, f1
    add.s   f3, f3, f2
    add.s   f3, f3, f9
    add.s   f3, f3, f11

    /* Pair48 multiply y * inner_sum. */
    mul.s   f5, f12, f0
    mov.s   f6, f5
    msub.s  f6, f12, f0
    mul.s   f2, f13, f3
    madd.s  f2, f12, f3
    madd.s  f2, f13, f0
    sub.s   f2, f2, f6

    /* Pair48 add exact Q=(0.5,0) to the product. */
    add.s   f6, f7, f5
    sub.s   f1, f6, f7
    sub.s   f4, f6, f1
    sub.s   f4, f7, f4
    sub.s   f5, f5, f1
    add.s   f4, f4, f5
    add.s   f4, f4, f2
    ee.stf.64.ip f4, f6, a2, 8
.endm

.align 4
.global pair48_k1_hydro_asm
.type pair48_k1_hydro_asm, @function
pair48_k1_hydro_asm:
    entry   a1, 16
    mov.n   a8, a2              /* restart x */
    mov.n   a9, a3              /* restart y */
    mov.n   a10, a4             /* restart z+10 */
    mov.n   a11, a5             /* n */
    mov.n   a12, a6             /* passes */
    bltz    a11, .Lk1_done
    beqz    a11, .Lk1_done
    bltz    a12, .Lk1_done
    beqz    a12, .Lk1_done

    /* Exact FP32 encodings of 0.125, 0.25 and 0.5; all low parts are zero. */
    movi    a13, 0x3e0
    slli    a13, a13, 20
    wfr     f14, a13
    movi    a13, 0x3e8
    slli    a13, a13, 20
    wfr     f15, a13
    movi    a13, 0x3f0
    slli    a13, a13, 20
    wfr     f7, a13

.Lk1_pass:
    mov.n   a2, a8
    mov.n   a3, a9
    mov.n   a4, a10
    /* Prologue: fetch z[k+10] and z[k+11], then use a sliding window. */
    ee.ldf.64.ip f9, f8, a4, 8
    ee.ldf.64.ip f11, f10, a4, 8
    ee.ldf.64.ip f13, f12, a3, 8
    mul.s   f8, f8, f14
    mul.s   f9, f9, f14
    mul.s   f10, f10, f15
    mul.s   f11, f11, f15
    PAIR48_K1_CALC

    /* The prologue handled one element; loop over the remaining n-1. */
    addi    a5, a11, -1
    beqz    a5, .Lk1_pass_done
    bnez    a5, .Lk1_sliding_loop
    j       .Lk1_pass_done
    .align 4
.Lk1_sliding_loop:
    loopnez a5, .Lk1_sliding_end
    /* 0.25*z[k+11] / 2 is exactly 0.125*z[k+11] for these normal inputs. */
    mul.s   f8, f10, f7
    mul.s   f9, f11, f7
    /* One new z Pair48 per element; old z[k+11] slides into z[k+10]. */
    ee.ldf.64.ip f11, f10, a4, 8
    mul.s   f10, f10, f15
    mul.s   f11, f11, f15
    ee.ldf.64.ip f13, f12, a3, 8
    PAIR48_K1_CALC
.Lk1_sliding_end:
.Lk1_pass_done:
    addi    a12, a12, -1
    bnez    a12, .Lk1_pass
    retw.n
.Lk1_done:
    retw.n
.size pair48_k1_hydro_asm, .-pair48_k1_hydro_asm
.purgem PAIR48_K1_CALC
