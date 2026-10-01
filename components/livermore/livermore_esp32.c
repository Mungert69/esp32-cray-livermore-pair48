
/*
 * ESP32-S3 Livermore Kernels 1-14 benchmark
 *
 * Uses the official Netlib C translation of the Livermore loop structures.
 * Three arithmetic modes:
 *   FP32     - native float, padded to a 64-bit slot to keep memory traffic
 *              comparable with Pair48/Cray words.
 *   Pair48   - two-float Lange/Rump-style pair arithmetic, no per-op normalization.
 *   Accurate - normalized double-single arithmetic (~48-bit significand target).
 *
 * Fidelity status:
 * - Kernel data follow McMahon's SIGNEL/SUPPLY/VALUES initialization for
 *   kernels 1-14, inner repeat loops preserve state exactly as the LFK TEST
 *   driver does, and checksums use the original ordinal SUMO slices. Only
 *   kernels 1-14 and one timing trial are run; the full seven-trial
 *   driver/statistics are not yet ported.
 * - Original ISPAN/IPASS/MUL and FLOPN/TOTAL accounting are preserved for K1-K14.
 * - The official LFK requires reporting a range, not one headline score.
 */

#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

#include "pair48_asm.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_timer.h"
#else
#include <time.h>
#define IRAM_ATTR
#define DRAM_ATTR
static int64_t esp_timer_get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec*1000000LL + ts.tv_nsec/1000;
}
#endif

#ifndef LFK_TIMING_MULT
#define LFK_TIMING_MULT 1
#endif

/* The K1 assembly experiment hardcodes coefficients from the old test data. */
#ifndef LFK_ENABLE_K1_ASM
#define LFK_ENABLE_K1_ASM 0
#endif

typedef pair48_t ds48_t;

/* Prevent the compiler from deleting or merging declared timing passes. */
#define LFK_PASS_BARRIER() __asm__ volatile ("" ::: "memory")

/* Native FMA helpers. On ESP32-S3 these force MADD.S/MSUB.S. */
static inline __attribute__((always_inline)) float fma_s(float acc, float a, float b)
{
#ifdef __XTENSA__
    __asm__("madd.s %0, %1, %2" : "+f"(acc) : "f"(a), "f"(b));
    return acc;
#else
    return fmaf(a,b,acc);
#endif
}
static inline __attribute__((always_inline)) float fms_s(float acc, float a, float b)
{
#ifdef __XTENSA__
    __asm__("msub.s %0, %1, %2" : "+f"(acc) : "f"(a), "f"(b));
    return acc;
#else
    return fmaf(-a,b,acc);
#endif
}

/* Convert a double reference value to normalized two-float form. */
static inline ds48_t ds_from_double(double x)
{
    float hi=(float)x;
    float lo=(float)(x-(double)hi);
    return (ds48_t){hi,lo};
}
static inline double ds_to_double(ds48_t x) { return (double)x.hi+(double)x.lo; }
static inline ds48_t ds_neg(ds48_t x) { return (ds48_t){-x.hi,-x.lo}; }

/* Park-Miller step and LFK IQRANF(IX,1,1001,n) mapping. */
static uint32_t lfk_iqranf_index(uint32_t *state)
{
    const uint32_t modulus=2147483647u;
    *state=(uint32_t)(((uint64_t)*state*16807u)%modulus);
    return 1u+(uint32_t)(((uint64_t)*state*1000u)/modulus);
}

static int lfk_iqranf_reference_check(void)
{
    static const uint16_t expected[30]={
        3,674,435,415,389,54,44,790,900,282,
        177,971,728,851,687,604,815,971,155,112,
        877,814,779,192,619,894,544,404,496,505
    };
    uint32_t state=256;
    for(size_t i=0;i<30;i++)
        if(lfk_iqranf_index(&state)!=expected[i]) return 0;
    return 1;
}

/* Pair arithmetic. */
static inline __attribute__((always_inline)) ds48_t pair_mul(ds48_t x, ds48_t y)
{
    float p=x.hi*y.hi;
    float e=fms_s(p,x.hi,y.hi);
    float t=x.lo*y.lo;
    t=fma_s(t,x.hi,y.lo);
    t=fma_s(t,x.lo,y.hi);
    t=t-e;
    return (ds48_t){p,t};
}
static inline __attribute__((always_inline)) ds48_t pair_add(ds48_t x, ds48_t y)
{
    float s=x.hi+y.hi;
    float v=s-x.hi;
    float lo=(x.hi-(s-v))+(y.hi-v)+x.lo+y.lo;
    return (ds48_t){s,lo};
}
static inline __attribute__((always_inline)) ds48_t pair_sub(ds48_t x, ds48_t y)
{
    return pair_add(x,ds_neg(y));
}

/* Accurate normalized double-single arithmetic. */
static inline __attribute__((always_inline)) ds48_t two_sum(float a,float b)
{
    float hi=a+b;
    float a1=hi-b, b1=hi-a1;
    return (ds48_t){hi,(a-a1)+(b-b1)};
}
static inline __attribute__((always_inline)) ds48_t fast_two_sum(float a,float b)
{
    float hi=a+b;
    float z=hi-a;
    return (ds48_t){hi,b-z};
}
static inline __attribute__((always_inline)) ds48_t acc_mul(ds48_t x,ds48_t y)
{
    float p=x.hi*y.hi;
    float neg_error=fms_s(p,x.hi,y.hi);
    float t=x.lo*y.lo;
    t=fma_s(t,x.hi,y.lo);
    t=fma_s(t,x.lo,y.hi);
    return fast_two_sum(p,t-neg_error);
}
static inline __attribute__((always_inline)) ds48_t acc_add(ds48_t x,ds48_t y)
{
    ds48_t s=two_sum(x.hi,y.hi);
    ds48_t t=two_sum(x.lo,y.lo);
    ds48_t v=fast_two_sum(s.hi,s.lo+t.hi);
    return fast_two_sum(v.hi,t.lo+v.lo);
}
static inline __attribute__((always_inline)) ds48_t acc_sub(ds48_t x,ds48_t y)
{
    return acc_add(x,ds_neg(y));
}

/*
 * FP32 mode uses an 8-byte slot intentionally. This keeps the same element
 * footprint as Pair48 and a Cray 64-bit word, so it is a compute-precision
 * reference rather than a peak 4-byte-FP32 bandwidth benchmark.
 */
typedef struct { float hi, pad; } fp32_slot_t;
static inline fp32_slot_t fp_from_double(double x){ return (fp32_slot_t){(float)x,0}; }
static inline double fp_to_double(fp32_slot_t x){ return x.hi; }
static inline fp32_slot_t fp_add(fp32_slot_t a,fp32_slot_t b){ return (fp32_slot_t){a.hi+b.hi,0}; }
static inline fp32_slot_t fp_sub(fp32_slot_t a,fp32_slot_t b){ return (fp32_slot_t){a.hi-b.hi,0}; }
static inline fp32_slot_t fp_mul(fp32_slot_t a,fp32_slot_t b){ return (fp32_slot_t){a.hi*b.hi,0}; }

/* Instantiate the exact same loop structures three times. */
#define LFK_SUFFIX fp32
#define LFK_T fp32_slot_t
#define LFK_FROM_D(x) fp_from_double((double)(x))
#define LFK_FROM_LONG(x) fp_from_double((double)(x))
#define LFK_TO_DOUBLE(x) fp_to_double(x)
#define LFK_TO_LONG(x) ((long)((x).hi))
#define LFK_ADD(a,b) fp_add((a),(b))
#define LFK_SUB(a,b) fp_sub((a),(b))
#define LFK_MUL(a,b) fp_mul((a),(b))
#include "livermore_kernels_1_14.inc"
#undef LFK_SUFFIX
#undef LFK_T
#undef LFK_FROM_D
#undef LFK_FROM_LONG
#undef LFK_TO_DOUBLE
#undef LFK_TO_LONG
#undef LFK_ADD
#undef LFK_SUB
#undef LFK_MUL

#define LFK_SUFFIX pair48
#define LFK_T ds48_t
#define LFK_FROM_D(x) ds_from_double((double)(x))
#define LFK_FROM_LONG(x) ds_from_double((double)(x))
#define LFK_TO_DOUBLE(x) ds_to_double(x)
#define LFK_TO_LONG(x) ((long)ds_to_double(x))
#define LFK_ADD(a,b) pair_add((a),(b))
#define LFK_SUB(a,b) pair_sub((a),(b))
#define LFK_MUL(a,b) pair_mul((a),(b))
#include "livermore_kernels_1_14.inc"
#undef LFK_SUFFIX
#undef LFK_T
#undef LFK_FROM_D
#undef LFK_FROM_LONG
#undef LFK_TO_DOUBLE
#undef LFK_TO_LONG
#undef LFK_ADD
#undef LFK_SUB
#undef LFK_MUL

#define LFK_SUFFIX acc48
#define LFK_T ds48_t
#define LFK_FROM_D(x) ds_from_double((double)(x))
#define LFK_FROM_LONG(x) ds_from_double((double)(x))
#define LFK_TO_DOUBLE(x) ds_to_double(x)
#define LFK_TO_LONG(x) ((long)ds_to_double(x))
#define LFK_ADD(a,b) acc_add((a),(b))
#define LFK_SUB(a,b) acc_sub((a),(b))
#define LFK_MUL(a,b) acc_mul((a),(b))
#include "livermore_kernels_1_14.inc"
#undef LFK_SUFFIX
#undef LFK_T
#undef LFK_FROM_D
#undef LFK_FROM_LONG
#undef LFK_TO_DOUBLE
#undef LFK_TO_LONG
#undef LFK_ADD
#undef LFK_SUB
#undef LFK_MUL

typedef union {
    lfk_ws_fp32 fp32;
    lfk_ws_pair48 pair48;
    lfk_ws_acc48 acc48;
} all_ws_t;

static DRAM_ATTR all_ws_t g_ws;

static const char *kernel_name[14]={
    "Hydro fragment","ICCG excerpt","Inner product","Banded equations",
    "Tri-diagonal","Linear recurrence","Equation of state","ADI integration",
    "Integrate predictors","Difference predictors","First sum","First difference",
    "2-D PIC","1-D PIC"
};

/* Original LFK controls, kernels 1-14 only. */
static const int ISPAN[3][14]={
    {1001,101,1001,1001,1001,64,995,100,101,101,1001,1000,64,1001},
    {101,101,101,101,101,32,101,100,101,101,101,100,32,101},
    {27,15,27,27,27,8,21,14,15,15,27,26,8,27}
};
static const int IPASS[3][14]={
    {7,67,9,14,10,3,4,10,36,34,11,12,36,2},
    {40,40,53,70,55,7,22,6,21,19,64,68,41,10},
    {28,46,37,38,40,21,20,9,26,25,46,48,31,8}
};
static const int MULSPAN[3]={1,2,8};
static const double FLOPN[14]={5,4,2,2,2,2,16,36,17,9,1,1,7,11};

/* McMahon mf528 BLOCK DATA checksums SUMS(:,span,1), MULTI=1, from Netlib. */
static const double LFK_SUMS_MULTI1[14][3]={
    {5.114652693224671e4,5.253344778937972e2,3.855104502494961e1},
    {1.539721811668385e3,1.539721811668385e3,3.953296986903059e1},
    {1.000742883066363e1,1.009741436578952e0,2.699309089320672e-1},
    {5.999250595473891e-1,5.999250595473891e-1,5.999250595473891e-1},
    {4.548871642387267e3,4.589031939600982e1,3.182615248447483e0},
    {4.375116344729986e3,8.631675645333210e1,1.120309393467088e0},
    {6.104251075174761e4,6.345586315784055e2,2.845720217644024e1},
    {1.501268005625798e5,1.501268005625798e5,2.960543667875003e3},
    {1.189443609974981e5,1.189443609974981e5,2.623968460874250e3},
    {7.310369784325296e4,7.310369784325296e4,1.651291227698265e3},
    {3.342910972650109e7,3.433560407475758e4,6.551161335845770e2},
    {2.907141294167248e-5,7.127569130821465e-6,1.943435981130448e-6},
    {4.468741170140841e8,2.323352389500009e8,4.143805389489125e7},
    {3.165006253912748e9,2.992144295804055e7,2.297991960376787e6}
};

static double original_total_units(int kernel,int n,int loops)
{
    double np=(double)loops, nn=(double)n;
    switch(kernel) {
    case 1: return np*nn;
    case 2: return np*(nn-4);
    case 3: return np*nn;
    case 4: return np*(((n-5)/5)+1)*3.0;
    case 5: return np*(nn-1);
    case 6: return np*nn*((nn-1)/2.0);
    case 7: return np*nn;
    case 8: return np*(nn-1)*2.0;
    case 9: return np*nn;
    case 10:return np*nn;
    case 11:return np*(nn-1);
    case 12:return np*nn;
    case 13:return np*nn;
    case 14:return np*nn;
    }
    return 0;
}

typedef enum { MODE_FP32, MODE_PAIR48, MODE_ACC48 } mode_t;

typedef struct {
    double mflops;
    double checksum;
    double us;
} result_t;

static result_t run_one(mode_t mode,int kernel,int span)
{
    int n=ISPAN[span][kernel-1];
    int loops=IPASS[span][kernel-1]*MULSPAN[span]*LFK_TIMING_MULT;

    int64_t t0,t1;
    double checksum;
    switch(mode) {
    case MODE_FP32:
        init_fp32(kernel,span,&g_ws.fp32);
        t0=esp_timer_get_time();
        run_fp32(kernel,n,loops,&g_ws.fp32);
        t1=esp_timer_get_time();
        checksum=checksum_fp32(kernel,n,&g_ws.fp32);
        break;
    case MODE_PAIR48:
        init_pair48(kernel,span,&g_ws.pair48);
        t0=esp_timer_get_time();
        run_pair48(kernel,n,loops,&g_ws.pair48);
        t1=esp_timer_get_time();
        checksum=checksum_pair48(kernel,n,&g_ws.pair48);
        break;
    default:
        init_acc48(kernel,span,&g_ws.acc48);
        t0=esp_timer_get_time();
        run_acc48(kernel,n,loops,&g_ws.acc48);
        t1=esp_timer_get_time();
        checksum=checksum_acc48(kernel,n,&g_ws.acc48);
        break;
    }

    double us=(double)(t1-t0);
    double useful_flops=FLOPN[kernel-1]*original_total_units(kernel,n,loops);
    result_t r={ useful_flops/us, checksum, us };
    return r;
}

/* Elementwise audit uses the same checksum-contributing Fortran storage slices
   as SUMO. It runs after timing, so the audit does not alter reported rates. */
#define ACC_AUDIT_CAPACITY 3000
static pair48_t audit_pair_values[ACC_AUDIT_CAPACITY];

static int collect_pair_acc_slices(int accurate,int kernel,int n,
                                   pair48_t *out[3],int lengths[3])
{
    int count=0;
#define ADD_SLICE(pair_ptr,acc_ptr,len) do { \
    out[count]=(accurate ? (pair48_t *)(acc_ptr) : (pair48_t *)(pair_ptr)); \
    lengths[count]=(len); count++; \
} while(0)
    switch(kernel) {
    case 1: ADD_SLICE(g_ws.pair48.k1.x,g_ws.acc48.k1.x,n); break;
    case 2: ADD_SLICE(g_ws.pair48.k2.x,g_ws.acc48.k2.x,2*n); break;
    case 3: ADD_SLICE(&g_ws.pair48.k3.q,&g_ws.acc48.k3.q,1); break;
    case 4: ADD_SLICE(g_ws.pair48.k4.v,g_ws.acc48.k4.v,3); break;
    case 5: ADD_SLICE(g_ws.pair48.k5.x+1,g_ws.acc48.k5.x+1,n-1); break;
    case 6: ADD_SLICE(g_ws.pair48.k6.w,g_ws.acc48.k6.w,n); break;
    case 7: ADD_SLICE(g_ws.pair48.k7.x,g_ws.acc48.k7.x,n); break;
    case 8:
        ADD_SLICE(&g_ws.pair48.k8.u1[0][0][0],&g_ws.acc48.k8.u1[0][0][0],10*n);
        ADD_SLICE(&g_ws.pair48.k8.u2[0][0][0],&g_ws.acc48.k8.u2[0][0][0],10*n);
        ADD_SLICE(&g_ws.pair48.k8.u3[0][0][0],&g_ws.acc48.k8.u3[0][0][0],10*n);
        break;
    case 9: ADD_SLICE(&g_ws.pair48.k9.px[0][0],&g_ws.acc48.k9.px[0][0],15*n); break;
    case 10: ADD_SLICE(&g_ws.pair48.k10.px[0][0],&g_ws.acc48.k10.px[0][0],15*n); break;
    case 11: ADD_SLICE(g_ws.pair48.k11.x+1,g_ws.acc48.k11.x+1,n-1); break;
    case 12: ADD_SLICE(g_ws.pair48.k12.x,g_ws.acc48.k12.x,n-1); break;
    case 13:
        ADD_SLICE(&g_ws.pair48.k13.p[0][0],&g_ws.acc48.k13.p[0][0],8*n);
        ADD_SLICE(&g_ws.pair48.k13.h[0][0],&g_ws.acc48.k13.h[0][0],8*n);
        break;
    case 14:
        ADD_SLICE(g_ws.pair48.k14.vx,g_ws.acc48.k14.vx,n);
        ADD_SLICE(g_ws.pair48.k14.xx,g_ws.acc48.k14.xx,n);
        ADD_SLICE(g_ws.pair48.k14.rh,g_ws.acc48.k14.rh,67);
        break;
    }
#undef ADD_SLICE
    return count;
}

static void audit_pair48_against_acc48(void)
{
    puts("\nPair48 vs Accurate48 element audit (checksum-contributing slices)");
    puts("checksum_rel is final checksum delta; repr/value_diff count differing representations/hi+lo values.");
    puts("max_abs/max_rel and worst_index span the concatenated SUMO slices; hi/lo are Pair48 minus Accurate48 there.");
    puts("K Span checksum_rel repr_diff value_diff max_abs max_rel worst_index hi_diff lo_diff");
    for(int span=0;span<3;span++) for(int kernel=1;kernel<=14;kernel++) {
        int n=ISPAN[span][kernel-1];
        int loops=IPASS[span][kernel-1]*MULSPAN[span]*LFK_TIMING_MULT;
        init_pair48(kernel,span,&g_ws.pair48);
        run_pair48(kernel,n,loops,&g_ws.pair48);
        double pair_checksum=checksum_pair48(kernel,n,&g_ws.pair48);
        pair48_t *pair_slices[3]; int pair_lengths[3];
        int segments=collect_pair_acc_slices(0,kernel,n,pair_slices,pair_lengths);
        int total=0;
        for(int seg=0;seg<segments;seg++) {
            if(total+pair_lengths[seg]>ACC_AUDIT_CAPACITY) {
                printf("%2d %d AUDIT_CAPACITY_EXCEEDED\n",kernel,span+1);
                total=0; break;
            }
            memcpy(audit_pair_values+total,pair_slices[seg],
                   (size_t)pair_lengths[seg]*sizeof(pair48_t));
            total+=pair_lengths[seg];
        }
        if(total==0) continue;

        init_acc48(kernel,span,&g_ws.acc48);
        run_acc48(kernel,n,loops,&g_ws.acc48);
        double acc_checksum=checksum_acc48(kernel,n,&g_ws.acc48);
        pair48_t *acc_slices[3]; int acc_lengths[3];
        int acc_segments=collect_pair_acc_slices(1,kernel,n,acc_slices,acc_lengths);
        int repr_diff=0,value_diff=0,worst=-1;
        double max_abs=0.0,max_rel=0.0,worst_hi=0.0,worst_lo=0.0;
        int index=0;
        for(int seg=0;seg<segments;seg++) {
            if(seg>=acc_segments || pair_lengths[seg]!=acc_lengths[seg]) break;
            for(int i=0;i<pair_lengths[seg];i++,index++) {
                pair48_t p=audit_pair_values[index],a=acc_slices[seg][i];
                if(memcmp(&p,&a,sizeof(p))!=0) repr_diff++;
                double pv=ds_to_double(p),av=ds_to_double(a),err=fabs(pv-av);
                if(pv!=av) value_diff++;
                double rel=err/(fabs(av)+1e-300);
                if(err>max_abs) max_abs=err;
                /* Retain the non-identical element maximizing relative error. */
                if(err>0.0 && (worst<0 || rel>max_rel)) {
                    worst=index; worst_hi=(double)p.hi-a.hi;
                    worst_lo=(double)p.lo-a.lo;
                }
                if(rel>max_rel) max_rel=rel;
            }
        }
        double checksum_rel=fabs(pair_checksum-acc_checksum)/(fabs(acc_checksum)+1e-300);
        printf("%2d %4d %.3e %d %d %.3e %.3e %d %.3e %.3e\n",
               kernel,span+1,checksum_rel,repr_diff,value_diff,max_abs,max_rel,
               worst,worst_hi,worst_lo);
    }
}

#if LFK_ENABLE_K1_ASM
static result_t run_pair48_k1_asm(int span)
{
    const int kernel=1;
    int n=ISPAN[span][kernel-1];
    int loops=IPASS[span][kernel-1]*MULSPAN[span]*LFK_TIMING_MULT;

    init_pair48(kernel,span,&g_ws.pair48);
    int64_t t0=esp_timer_get_time();
    pair48_k1_hydro_asm(g_ws.pair48.k1.x,
                        g_ws.pair48.k1.y,
                        &g_ws.pair48.k1.z[10],
                        n,
                        loops);
    int64_t t1=esp_timer_get_time();

    double us=(double)(t1-t0);
    double checksum=checksum_pair48(kernel,n,&g_ws.pair48);
    double useful_flops=FLOPN[kernel-1]*original_total_units(kernel,n,loops);
    result_t r={ useful_flops/us, checksum, us };
    return r;
}
#endif

typedef struct {
    uint8_t i1, j1;             /* first field lookup */
    uint8_t lookup_i, lookup_j; /* second field lookup, before e/f offset */
    uint8_t cell_i, cell_j;     /* final histogram cell */
} k13_index_record_t;

/* Largest span is 41 * 2 * timing-multiplier(4) * n(32) = 10,496 records. */
static k13_index_record_t k13_pair_trace[11000];

static void report_k13_index_paths(int span)
{
    const int n=ISPAN[span][12];
    const int loops=IPASS[span][12]*MULSPAN[span]*LFK_TIMING_MULT;
    int mismatches_first=0, mismatches_lookup=0, mismatches_cell=0;
    int first_pass=-1, first_particle=-1;
    k13_index_record_t first_pair={0}, first_acc={0};
    size_t count=0;

    for(int accurate=0;accurate<2;accurate++) {
        if(accurate) init_acc48(13,span,&g_ws.acc48);
        else init_pair48(13,span,&g_ws.pair48);

        pair48_t (*p)[4]=accurate ? g_ws.acc48.k13.p : g_ws.pair48.k13.p;
        pair48_t (*b)[64]=accurate ? g_ws.acc48.k13.b : g_ws.pair48.k13.b;
        pair48_t (*c)[64]=accurate ? g_ws.acc48.k13.c : g_ws.pair48.k13.c;
        pair48_t (*h)[64]=accurate ? g_ws.acc48.k13.h : g_ws.pair48.k13.h;
        pair48_t *y=accurate ? g_ws.acc48.k13.y : g_ws.pair48.k13.y;
        pair48_t *z=accurate ? g_ws.acc48.k13.z : g_ws.pair48.k13.z;
        int32_t *e=accurate ? g_ws.acc48.k13.e : g_ws.pair48.k13.e;
        int32_t *f=accurate ? g_ws.acc48.k13.f : g_ws.pair48.k13.f;
        pair48_t one=ds_from_double(1.0);

        for(int pass=0;pass<loops;pass++) {
            for(int ip=0;ip<n;ip++) {
                int i1=(int)p[ip][0].hi & 63;
                int j1=(int)p[ip][1].hi & 63;
                p[ip][2]=accurate ? acc_add(p[ip][2],b[j1][i1]) : pair_add(p[ip][2],b[j1][i1]);
                p[ip][3]=accurate ? acc_add(p[ip][3],c[j1][i1]) : pair_add(p[ip][3],c[j1][i1]);
                p[ip][0]=accurate ? acc_add(p[ip][0],p[ip][2]) : pair_add(p[ip][0],p[ip][2]);
                p[ip][1]=accurate ? acc_add(p[ip][1],p[ip][3]) : pair_add(p[ip][1],p[ip][3]);
                int i2=((int)p[ip][0].hi & 63)-1;
                int j2=((int)p[ip][1].hi & 63)-1;

                k13_index_record_t rec={
                    (uint8_t)i1,(uint8_t)j1,(uint8_t)i2,(uint8_t)j2,0,0
                };
                p[ip][0]=accurate ? acc_add(p[ip][0],y[i2+32]) : pair_add(p[ip][0],y[i2+32]);
                p[ip][1]=accurate ? acc_add(p[ip][1],z[j2+32]) : pair_add(p[ip][1],z[j2+32]);
                i2+=e[i2+32];
                j2+=f[j2+32];
                rec.cell_i=(uint8_t)i2;
                rec.cell_j=(uint8_t)j2;
                h[j2][i2]=accurate ? acc_add(h[j2][i2],one) : pair_add(h[j2][i2],one);

                size_t at=(size_t)pass*n+ip;
                if(!accurate) k13_pair_trace[at]=rec;
                else {
                    k13_index_record_t old=k13_pair_trace[at];
                    if(old.i1!=rec.i1 || old.j1!=rec.j1) mismatches_first++;
                    if(old.lookup_i!=rec.lookup_i || old.lookup_j!=rec.lookup_j) mismatches_lookup++;
                    if(old.cell_i!=rec.cell_i || old.cell_j!=rec.cell_j) mismatches_cell++;
                    if(first_pass<0 && (old.i1!=rec.i1 || old.j1!=rec.j1 ||
                       old.lookup_i!=rec.lookup_i || old.lookup_j!=rec.lookup_j ||
                       old.cell_i!=rec.cell_i || old.cell_j!=rec.cell_j)) {
                        first_pass=pass;
                        first_particle=ip;
                        first_pair=old;
                        first_acc=rec;
                    }
                    count++;
                }
            }
        }
    }

    printf("K13 index path span %d: %u particle-passes; mismatches first=%d, "
           "second-lookup=%d, deposit-cell=%d\n",
           span+1,(unsigned)count,mismatches_first,mismatches_lookup,mismatches_cell);
    if(first_pass>=0)
        printf("  first divergence pass=%d particle=%d Pair48 (%u,%u)->(%u,%u)->(%u,%u), "
               "Accurate48 (%u,%u)->(%u,%u)->(%u,%u)\n",
               first_pass,first_particle,
               first_pair.i1,first_pair.j1,first_pair.lookup_i,first_pair.lookup_j,
               first_pair.cell_i,first_pair.cell_j,
               first_acc.i1,first_acc.j1,first_acc.lookup_i,first_acc.lookup_j,
               first_acc.cell_i,first_acc.cell_j);
}

static void run_suite(void)
{
    printf("\nESP32-S3 Livermore Kernels 1-14\n");
    printf("IQRANF seed-256 reference vector: %s\n",
           lfk_iqranf_reference_check() ? "PASS" : "FAIL");
    printf("Official C loop structures; original 3 span sets and LFK useful-FLOP accounting\n");
    printf("Timing multiplier: %d\n\n",LFK_TIMING_MULT);

    double sum_pair=0,sum_acc=0,sum_fp=0;
    int count=0;

    for(int span=0;span<3;span++) {
        printf("=== SPAN SET %d ===\n",span+1);
        printf(" K  %-21s %5s %10s %10s %10s  pair/acc err  Accurate48 checksum / LFK SUMS\n",
               "kernel","n","FP32","Pair48","Acc48");

        for(int k=1;k<=14;k++) {
            result_t fp=run_one(MODE_FP32,k,span);
            result_t pa=run_one(MODE_PAIR48,k,span);
            result_t ac=run_one(MODE_ACC48,k,span);

            double rel=fabs(pa.checksum-ac.checksum)/(fabs(ac.checksum)+1e-300);
            double proof_rel=fabs(ac.checksum-LFK_SUMS_MULTI1[k-1][span])/
                             (fabs(LFK_SUMS_MULTI1[k-1][span])+1e-300);
            printf("%2d  %-21s %5d %9.3fM %9.3fM %9.3fM  %.3e  %.9e / %.9e (%s%.3e)\n",
                   k,kernel_name[k-1],ISPAN[span][k-1],
                   fp.mflops,pa.mflops,ac.mflops,rel,ac.checksum,LFK_SUMS_MULTI1[k-1][span],
                   LFK_TIMING_MULT==1 ? "" : "not-comparable; ",proof_rel);

            if(k==1) {
#if LFK_ENABLE_K1_ASM
                result_t opt=run_pair48_k1_asm(span);
                double opt_rel=fabs(opt.checksum-ac.checksum)/
                               (fabs(ac.checksum)+1e-300);
                printf("    Pair48 ASM whole K1: %.3fM (%.1f us), %.2fx vs C, "
                       "Acc48 rel.err %.3e\n",
                       opt.mflops,opt.us,opt.mflops/pa.mflops,opt_rel);
#endif
            }

            sum_fp+=log(fp.mflops+1e-300);
            sum_pair+=log(pa.mflops+1e-300);
            sum_acc+=log(ac.mflops+1e-300);
            count++;
        }
        printf("\n");
    }

    for(int span=0;span<3;span++) report_k13_index_paths(span);
    audit_pair48_against_acc48();

    printf("Geometric mean over 42 samples:\n");
    printf("  FP32 padded : %.3f MFLOPS\n",exp(sum_fp/count));
    printf("  Pair48      : %.3f MFLOPS\n",exp(sum_pair/count));
    printf("  Accurate48  : %.3f MFLOPS\n",exp(sum_acc/count));
    printf("\nLFK reports a performance RANGE; do not reduce this suite to only one number.\n");
    printf("For Cray-1 comparison, use historical per-kernel/span data where available.\n");
}

#ifdef ESP_PLATFORM
void livermore_app_main(void) { run_suite(); }
#else
int main(void) { run_suite(); return 0; }
#endif
