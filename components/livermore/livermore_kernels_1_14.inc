
/*
 * Parameterized Livermore Kernels 1-14.
 *
 * Required macros before inclusion:
 *   LFK_SUFFIX
 *   LFK_T
 *   LFK_FROM_D(x)
 *   LFK_FROM_LONG(x)
 *   LFK_TO_DOUBLE(x)
 *   LFK_TO_LONG(x)
 *   LFK_ADD(a,b)
 *   LFK_SUB(a,b)
 *   LFK_MUL(a,b)
 *
 * Loop bodies follow Netlib's official C translation of the
 * Livermore Fortran Kernels, kernels 1-14.
 */

#define LFK_CAT2_(a,b) a##b
#define LFK_CAT_(a,b) LFK_CAT2_(a,b)
#define LFK_NAME_(a) LFK_CAT_(a, LFK_SUFFIX)

typedef union {
    struct { LFK_T x[1012], y[1012], z[1012]; } k1;
    struct { LFK_T x[1012], v[1012], x0[1012]; } k2;
    struct { LFK_T x[1012], z[1012]; LFK_T q; } k3;
    /* K4: original Fortran aliases XZ(1500) onto storage beginning at X(1).
       Keep X and Y contiguous explicitly so XZ can legally span X + first 499 Y values. */
    struct { LFK_T xy[2002], xy0[2002], v[3]; } k4;
    struct { LFK_T x[1012], y[1012], z[1012], x0[1012]; } k5;
    struct { LFK_T w[1012], b[64][64], w0[1012]; } k6;
    struct { LFK_T x[1012], u[1012], y[1012], z[1012]; } k7;
    struct {
        LFK_T du1[101], du2[101], du3[101];
        LFK_T u1[2][101][5], u2[2][101][5], u3[2][101][5];
    } k8;
    struct { LFK_T px[101][25]; } k9;
    struct { LFK_T px[101][25], cx[101][25], px0[101][9]; } k10;
    struct { LFK_T x[1012], y[1012]; } k11;
    struct { LFK_T x[1012], y[1012]; } k12;
    struct {
        LFK_T p[512][4];
        LFK_T b[64][64], c[64][64], h[64][64], h0[64][64];
        LFK_T y[100], z[100];
        int32_t e[96], f[96];
    } k13;
    struct {
        LFK_T grd[1001], dex[1001], xi[1001], ex[1001], ex1[1001], dex1[1001];
        LFK_T vx[1001], xx[1001], rx[1001], rh[2048], rh0[2048];
        int32_t ix[1001], ir[1001];
    } k14;
} LFK_NAME_(lfk_ws_);

/* McMahon's SIGNEL generator, preserving the source's double intermediates. */
static void LFK_NAME_(signel_fill_)(LFK_T *dst, int count, int offset)
{
    double scaled=0.1, fuzz=1.234500e-3;
    double buzz=1.0+fuzz, fizz=1.1*fuzz;
    int end=offset+count;
    for (int i=0;i<end;i++) {
        buzz=(1.0-fuzz)*buzz+fuzz;
        fuzz=-fuzz;
        if (i>=offset) dst[i-offset]=LFK_FROM_D((buzz-fizz)*scaled);
    }
}

static void LFK_NAME_(init_)(int kernel, int span, void *mem)
{
    LFK_NAME_(lfk_ws_) *ws = (LFK_NAME_(lfk_ws_) *)mem;
    int i, j;

    switch (kernel) {
    case 1:
        LFK_NAME_(signel_fill_)(ws->k1.x,1012,3003);
        LFK_NAME_(signel_fill_)(ws->k1.y,1012,4004);
        LFK_NAME_(signel_fill_)(ws->k1.z,1012,5005);
        break;
    case 2:
        LFK_NAME_(signel_fill_)(ws->k2.x,1012,3003);
        LFK_NAME_(signel_fill_)(ws->k2.v,1012,1001);
        memcpy(ws->k2.x0,ws->k2.x,sizeof(ws->k2.x));
        break;
    case 3:
        LFK_NAME_(signel_fill_)(ws->k3.x,1012,3003);
        LFK_NAME_(signel_fill_)(ws->k3.z,1012,5005);
        ws->k3.q=LFK_FROM_D(0.0);
        break;
    case 4:
        LFK_NAME_(signel_fill_)(ws->k4.xy,2002,3003);
        memcpy(ws->k4.xy0,ws->k4.xy,sizeof(ws->k4.xy));
        LFK_NAME_(signel_fill_)(ws->k4.v,3,1001);
        break;
    case 5:
        LFK_NAME_(signel_fill_)(ws->k5.x,1012,3003);
        LFK_NAME_(signel_fill_)(ws->k5.y,1012,4004);
        LFK_NAME_(signel_fill_)(ws->k5.z,1012,5005);
        memcpy(ws->k5.x0,ws->k5.x,sizeof(ws->k5.x));
        break;
    case 6:
        LFK_NAME_(signel_fill_)(ws->k6.w,1012,2002);
        LFK_NAME_(signel_fill_)(ws->k6.b[0],4096,18814);
        memcpy(ws->k6.w0,ws->k6.w,sizeof(ws->k6.w));
        break;
    case 7:
        LFK_NAME_(signel_fill_)(ws->k7.x,1012,3003);
        LFK_NAME_(signel_fill_)(ws->k7.u,1012,0);
        LFK_NAME_(signel_fill_)(ws->k7.y,1012,4004);
        LFK_NAME_(signel_fill_)(ws->k7.z,1012,5005);
        break;
    case 8:
        LFK_NAME_(signel_fill_)(ws->k8.du1,101,7007);
        LFK_NAME_(signel_fill_)(ws->k8.du2,101,7108);
        LFK_NAME_(signel_fill_)(ws->k8.du3,101,7209);
        LFK_NAME_(signel_fill_)(ws->k8.u1[0][0],1010,31102);
        LFK_NAME_(signel_fill_)(ws->k8.u2[0][0],1010,32112);
        LFK_NAME_(signel_fill_)(ws->k8.u3[0][0],1010,33122);
        break;
    case 9:
        LFK_NAME_(signel_fill_)(ws->k9.px[0],2525,2048);
        break;
    case 10:
        LFK_NAME_(signel_fill_)(ws->k10.px[0],2525,2048);
        LFK_NAME_(signel_fill_)(ws->k10.cx[0],2525,4573);
        for (i=0;i<101;i++) for(j=0;j<9;j++) ws->k10.px0[i][j]=ws->k10.px[i][j+4];
        break;
    case 11:
        LFK_NAME_(signel_fill_)(ws->k11.x,1012,3003);
        LFK_NAME_(signel_fill_)(ws->k11.y,1012,4004);
        break;
    case 12:
        LFK_NAME_(signel_fill_)(ws->k12.x,1012,3003);
        LFK_NAME_(signel_fill_)(ws->k12.y,1012,4004);
        break;
    case 13:
        for (i=0;i<512;i++) {
            for(j=0;j<4;j++) ws->k13.p[i][j]=LFK_FROM_D(1.0+0.5*(j*512+i));
        }
        LFK_NAME_(signel_fill_)(ws->k13.b[0],4096,18814);
        LFK_NAME_(signel_fill_)(ws->k13.c[0],4096,22910);
        LFK_NAME_(signel_fill_)(ws->k13.h[0],4096,27006);
        memcpy(ws->k13.h0,ws->k13.h,sizeof(ws->k13.h));
        LFK_NAME_(signel_fill_)(ws->k13.y,100,4004);
        LFK_NAME_(signel_fill_)(ws->k13.z,100,5005);
        for (i=0;i<96;i++) ws->k13.e[i]=ws->k13.f[i]=1;
        break;
    case 14:
        LFK_NAME_(signel_fill_)(ws->k14.grd,1001,7310);
        LFK_NAME_(signel_fill_)(ws->k14.dex,1001,8311);
        LFK_NAME_(signel_fill_)(ws->k14.xi,1001,9312);
        LFK_NAME_(signel_fill_)(ws->k14.ex,1001,10313);
        LFK_NAME_(signel_fill_)(ws->k14.ex1,1001,11314);
        LFK_NAME_(signel_fill_)(ws->k14.dex1,1001,12315);
        LFK_NAME_(signel_fill_)(ws->k14.vx,1001,13316);
        LFK_NAME_(signel_fill_)(ws->k14.xx,1001,14317);
        LFK_NAME_(signel_fill_)(ws->k14.rx,1001,15318);
        LFK_NAME_(signel_fill_)(ws->k14.rh,2048,16319);
        memcpy(ws->k14.rh0,ws->k14.rh,sizeof(ws->k14.rh));
        /* Original IQRANF(IX,1,1001,1001), with IQRAN0(256) at each test run. */
        uint32_t state=256;
        for(i=0;i<1001*(span+1);i++) {
            int index=(int)lfk_iqranf_index(&state);
            if(i/1001==span) ws->k14.grd[i%1001]=LFK_FROM_D((double)index);
        }
        for(i=0;i<1001;i++) ws->k14.dex[i]=LFK_MUL(LFK_FROM_D(-100.0),ws->k14.dex[i]);
        for(i=0;i<1001;i++) ws->k14.ix[i]=ws->k14.ir[i]=0;
        break;
    }
}

static IRAM_ATTR __attribute__((noinline,optimize("O3")))
void LFK_NAME_(run_)(int kernel, int n, int loops, void *mem)
{
    LFK_NAME_(lfk_ws_) *ws = (LFK_NAME_(lfk_ws_) *)mem;
    int l, k, i, j, ii, ipnt, ipntp, lw, nl1, nl2, kx, ky, ip;
    int i1, j1, i2, j2;
    const LFK_T Q   = LFK_FROM_D(0.099987652366113677);
    const LFK_T R   = LFK_FROM_D(0.099987652177981098);
    const LFK_T T   = LFK_FROM_D(0.099987651613585027);
    const LFK_T SIG = LFK_FROM_D(0.099987651801716759);
    const LFK_T TWO = LFK_FROM_D(2.0);
    const LFK_T ONE = LFK_FROM_D(1.0);
    const LFK_T FLX = LFK_FROM_D(0.001);

    const LFK_T A11=LFK_FROM_D(0.099987502600974998), A12=LFK_FROM_D(0.099987654811863405), A13=LFK_FROM_D(0.099987502413070667);
    const LFK_T A21=LFK_FROM_D(0.099987654623727107),A22=LFK_FROM_D(0.099987502225166600),  A23=LFK_FROM_D(0.099987654435591058);
    const LFK_T A31=LFK_FROM_D(0.099987502037262810),A32=LFK_FROM_D(0.099987654247455315), A33=LFK_FROM_D(0.099987501849359325);

    const LFK_T DM22=LFK_FROM_D(0.099987653494915216),DM23=LFK_FROM_D(0.099987501097748233),DM24=LFK_FROM_D(0.099987653306780902);
    const LFK_T DM25=LFK_FROM_D(0.099987500909846191),DM26=LFK_FROM_D(0.099987653118646880),DM27=LFK_FROM_D(0.099987500721944400);
    const LFK_T DM28=LFK_FROM_D(0.099987652930513149),C0=LFK_FROM_D(0.099987653871184690);

    switch (kernel) {
    case 1: /* Hydro fragment */
        for (l=0;l<loops;l++) {
            for (k=0;k<n;k++)
                ws->k1.x[k] = LFK_ADD(Q,
                    LFK_MUL(ws->k1.y[k],
                        LFK_ADD(LFK_MUL(R,ws->k1.z[k+10]),
                                LFK_MUL(T,ws->k1.z[k+11]))));
            LFK_PASS_BARRIER();
        }
        break;

    case 2: /* ICCG excerpt */
        for (l=0;l<loops;l++) {
            if(l>0) memcpy(ws->k2.x,ws->k2.x0,(size_t)n*sizeof(LFK_T));
            ii=n; ipntp=0;
            do {
                ipnt=ipntp; ipntp+=ii; ii/=2; i=ipntp;
                for (k=ipnt+1;k<ipntp;k+=2) {
                    i++;
                    ws->k2.x[i] = LFK_SUB(
                        LFK_SUB(ws->k2.x[k], LFK_MUL(ws->k2.v[k],ws->k2.x[k-1])),
                        LFK_MUL(ws->k2.v[k+1],ws->k2.x[k+1]));
                }
            } while (ii>1);
            LFK_PASS_BARRIER();
        }
        break;

    case 3: /* Inner product */
        for (l=0;l<loops;l++) {
            LFK_T qv=LFK_FROM_D(0.0);
            for (k=0;k<n;k++) qv=LFK_ADD(qv,LFK_MUL(ws->k3.z[k],ws->k3.x[k]));
            ws->k3.q=qv;
            LFK_PASS_BARRIER();
        }
        break;

    case 4: /* Banded linear equations */
        for (l=0;l<loops;l++) {
            if(l>0) for(i=0;i<3;i++) ws->k4.xy[(int[]){6,503,1000}[i]]=ws->k4.xy0[(int[]){6,503,1000}[i]];
            int m=(1001-7)/2;
            LFK_T *xz=ws->k4.xy;
            LFK_T *yy=&ws->k4.xy[1001];
            for (k=6;k<1001;k+=m) {
                lw=k-6;
                LFK_T temp=xz[k-1];
                for (j=4;j<n;j+=5) {
                    temp=LFK_SUB(temp,LFK_MUL(xz[lw],yy[j]));
                    lw++;
                }
                xz[k-1]=LFK_MUL(yy[4],temp);
            }
            LFK_PASS_BARRIER();
        }
        break;

    case 5: /* Tri-diagonal elimination */
        for (l=0;l<loops;l++) {
            if(l>0) memcpy(ws->k5.x,ws->k5.x0,(size_t)n*sizeof(LFK_T));
            for (i=1;i<n;i++)
                ws->k5.x[i]=LFK_MUL(ws->k5.z[i],LFK_SUB(ws->k5.y[i],ws->k5.x[i-1]));
            LFK_PASS_BARRIER();
        }
        break;

    case 6: /* General linear recurrence */
        for (l=0;l<loops;l++) {
            if(l>0) memcpy(ws->k6.w,ws->k6.w0,(size_t)n*sizeof(LFK_T));
            for (i=1;i<n;i++) {
                /* Original LFK resets W(i) before the recurrence. */
                ws->k6.w[i]=LFK_FROM_D(0.0100);
                for (k=0;k<i;k++)
                    ws->k6.w[i]=LFK_ADD(ws->k6.w[i],
                        LFK_MUL(ws->k6.b[k][i],ws->k6.w[(i-k)-1]));
            }
            LFK_PASS_BARRIER();
        }
        break;

    case 7: /* Equation of state */
        for (l=0;l<loops;l++) {
            for (k=0;k<n;k++) {
                LFK_T a = LFK_ADD(ws->k7.z[k],LFK_MUL(R,ws->k7.y[k]));
                LFK_T b = LFK_ADD(ws->k7.u[k+2],LFK_MUL(R,ws->k7.u[k+1]));
                b = LFK_ADD(ws->k7.u[k+3],LFK_MUL(R,b));
                LFK_T c = LFK_ADD(ws->k7.u[k+5],LFK_MUL(R,ws->k7.u[k+4]));
                c = LFK_ADD(ws->k7.u[k+6],LFK_MUL(R,c));
                ws->k7.x[k]=LFK_ADD(
                    LFK_ADD(ws->k7.u[k],LFK_MUL(R,a)),
                    LFK_MUL(T,LFK_ADD(b,LFK_MUL(T,c))));
            }
            LFK_PASS_BARRIER();
        }
        break;

    case 8: /* ADI integration */
        for (l=0;l<loops;l++) {
            nl1=0; nl2=1;
            for (kx=1;kx<3;kx++)
                for (ky=1;ky<n;ky++) {
                    ws->k8.du1[ky]=LFK_SUB(ws->k8.u1[nl1][ky+1][kx],ws->k8.u1[nl1][ky-1][kx]);
                    ws->k8.du2[ky]=LFK_SUB(ws->k8.u2[nl1][ky+1][kx],ws->k8.u2[nl1][ky-1][kx]);
                    ws->k8.du3[ky]=LFK_SUB(ws->k8.u3[nl1][ky+1][kx],ws->k8.u3[nl1][ky-1][kx]);

#define LFK_ADI(dst,src,A1,A2,A3,duA,duB,duC) do { \
    LFK_T vv=src[nl1][ky][kx]; \
    vv=LFK_ADD(vv,LFK_MUL((A1),(duA)[ky])); \
    vv=LFK_ADD(vv,LFK_MUL((A2),(duB)[ky])); \
    vv=LFK_ADD(vv,LFK_MUL((A3),(duC)[ky])); \
    LFK_T lap=LFK_ADD(LFK_SUB(src[nl1][ky][kx+1],LFK_MUL(TWO,src[nl1][ky][kx])),src[nl1][ky][kx-1]); \
    dst[nl2][ky][kx]=LFK_ADD(vv,LFK_MUL(SIG,lap)); \
} while(0)
                    LFK_ADI(ws->k8.u1,ws->k8.u1,A11,A12,A13,ws->k8.du1,ws->k8.du2,ws->k8.du3);
                    LFK_ADI(ws->k8.u2,ws->k8.u2,A21,A22,A23,ws->k8.du1,ws->k8.du2,ws->k8.du3);
                    LFK_ADI(ws->k8.u3,ws->k8.u3,A31,A32,A33,ws->k8.du1,ws->k8.du2,ws->k8.du3);
#undef LFK_ADI
                }
            LFK_PASS_BARRIER();
        }
        break;

    case 9: /* Integrate predictors */
        for (l=0;l<loops;l++) {
            for (i=0;i<n;i++) {
                LFK_T v=LFK_MUL(DM28,ws->k9.px[i][12]);
                v=LFK_ADD(v,LFK_MUL(DM27,ws->k9.px[i][11]));
                v=LFK_ADD(v,LFK_MUL(DM26,ws->k9.px[i][10]));
                v=LFK_ADD(v,LFK_MUL(DM25,ws->k9.px[i][9]));
                v=LFK_ADD(v,LFK_MUL(DM24,ws->k9.px[i][8]));
                v=LFK_ADD(v,LFK_MUL(DM23,ws->k9.px[i][7]));
                v=LFK_ADD(v,LFK_MUL(DM22,ws->k9.px[i][6]));
                v=LFK_ADD(v,LFK_MUL(C0,LFK_ADD(ws->k9.px[i][4],ws->k9.px[i][5])));
                ws->k9.px[i][0]=LFK_ADD(v,ws->k9.px[i][2]);
            }
            LFK_PASS_BARRIER();
        }
        break;

    case 10: /* Difference predictors */
        for (l=0;l<loops;l++) {
            for (i=0;i<n;i++) {
                LFK_T ar=ws->k10.cx[i][4];
                LFK_T br=LFK_SUB(ar,ws->k10.px[i][4]); ws->k10.px[i][4]=ar;
                LFK_T cr=LFK_SUB(br,ws->k10.px[i][5]); ws->k10.px[i][5]=br;
                ar=LFK_SUB(cr,ws->k10.px[i][6]); ws->k10.px[i][6]=cr;
                br=LFK_SUB(ar,ws->k10.px[i][7]); ws->k10.px[i][7]=ar;
                cr=LFK_SUB(br,ws->k10.px[i][8]); ws->k10.px[i][8]=br;
                ar=LFK_SUB(cr,ws->k10.px[i][9]); ws->k10.px[i][9]=cr;
                br=LFK_SUB(ar,ws->k10.px[i][10]);ws->k10.px[i][10]=ar;
                cr=LFK_SUB(br,ws->k10.px[i][11]);ws->k10.px[i][11]=br;
                ws->k10.px[i][13]=LFK_SUB(cr,ws->k10.px[i][12]);
                ws->k10.px[i][12]=cr;
            }
            LFK_PASS_BARRIER();
        }
        break;

    case 11: /* First sum */
        for (l=0;l<loops;l++) {
            ws->k11.x[0]=ws->k11.y[0];
            for (k=1;k<n;k++) ws->k11.x[k]=LFK_ADD(ws->k11.x[k-1],ws->k11.y[k]);
            LFK_PASS_BARRIER();
        }
        break;

    case 12: /* First difference */
        for (l=0;l<loops;l++) {
            for (k=0;k<n;k++)
                ws->k12.x[k]=LFK_SUB(ws->k12.y[k+1],ws->k12.y[k]);
            LFK_PASS_BARRIER();
        }
        break;

    case 13: /* 2-D PIC */
        for (l=0;l<loops;l++) {
            for (ip=0;ip<n;ip++) {
                i1=(int)LFK_TO_LONG(ws->k13.p[ip][0]) & 63;
                j1=(int)LFK_TO_LONG(ws->k13.p[ip][1]) & 63;
                ws->k13.p[ip][2]=LFK_ADD(ws->k13.p[ip][2],ws->k13.b[j1][i1]);
                ws->k13.p[ip][3]=LFK_ADD(ws->k13.p[ip][3],ws->k13.c[j1][i1]);
                ws->k13.p[ip][0]=LFK_ADD(ws->k13.p[ip][0],ws->k13.p[ip][2]);
                ws->k13.p[ip][1]=LFK_ADD(ws->k13.p[ip][1],ws->k13.p[ip][3]);
                i2=((int)LFK_TO_LONG(ws->k13.p[ip][0]) & 63)-1;
                j2=((int)LFK_TO_LONG(ws->k13.p[ip][1]) & 63)-1;
                ws->k13.p[ip][0]=LFK_ADD(ws->k13.p[ip][0],ws->k13.y[i2+32]);
                ws->k13.p[ip][1]=LFK_ADD(ws->k13.p[ip][1],ws->k13.z[j2+32]);
                i2+=ws->k13.e[i2+32];
                j2+=ws->k13.f[j2+32];
                ws->k13.h[j2][i2]=LFK_ADD(ws->k13.h[j2][i2],ONE);
            }
            LFK_PASS_BARRIER();
        }
        break;

    case 14: /* 1-D PIC */
        for (l=0;l<loops;l++) {
            for (k=0;k<n;k++) {
                ws->k14.vx[k]=LFK_FROM_D(0.0);
                ws->k14.xx[k]=LFK_FROM_D(0.0);
                ws->k14.ix[k]=(int32_t)LFK_TO_LONG(ws->k14.grd[k]);
                ws->k14.xi[k]=LFK_FROM_LONG(ws->k14.ix[k]);
                ws->k14.ex1[k]=ws->k14.ex[ws->k14.ix[k]-1];
                ws->k14.dex1[k]=ws->k14.dex[ws->k14.ix[k]-1];
            }
            for (k=0;k<n;k++) {
                LFK_T term=LFK_MUL(LFK_SUB(ws->k14.xx[k],ws->k14.xi[k]),ws->k14.dex1[k]);
                ws->k14.vx[k]=LFK_ADD(LFK_ADD(ws->k14.vx[k],ws->k14.ex1[k]),term);
                ws->k14.xx[k]=LFK_ADD(LFK_ADD(ws->k14.xx[k],ws->k14.vx[k]),FLX);
                ws->k14.ir[k]=(int32_t)LFK_TO_LONG(ws->k14.xx[k]);
                ws->k14.rx[k]=LFK_SUB(ws->k14.xx[k],LFK_FROM_LONG(ws->k14.ir[k]));
                ws->k14.ir[k]=(ws->k14.ir[k] & 2047)+1;
                ws->k14.xx[k]=LFK_ADD(ws->k14.rx[k],LFK_FROM_LONG(ws->k14.ir[k]));
            }
            for (k=0;k<n;k++) {
                int idx=ws->k14.ir[k]-1;
                ws->k14.rh[idx]=LFK_ADD(ws->k14.rh[idx],LFK_SUB(ONE,ws->k14.rx[k]));
                ws->k14.rh[idx+1]=LFK_ADD(ws->k14.rh[idx+1],ws->k14.rx[k]);
            }
            LFK_PASS_BARRIER();
        }
        break;
    }
}

static double LFK_NAME_(sumo_)(const LFK_T *v, int n)
{
    double s=0.0;
    for(int i=0;i<n;i++) s+=(double)(i+1)*LFK_TO_DOUBLE(v[i]);
    return s;
}

static double LFK_NAME_(checksum_)(int kernel, int n, void *mem)
{
    LFK_NAME_(lfk_ws_) *ws=(LFK_NAME_(lfk_ws_) *)mem;
    switch(kernel) {
    case 1: return LFK_NAME_(sumo_)(ws->k1.x,n);
    case 2: return LFK_NAME_(sumo_)(ws->k2.x,2*n);
    case 3: return LFK_TO_DOUBLE(ws->k3.q);
    case 4: return LFK_NAME_(sumo_)(ws->k4.v,3);
    case 5: return LFK_NAME_(sumo_)(ws->k5.x+1,n-1);
    case 6: return LFK_NAME_(sumo_)(ws->k6.w,n);
    case 7: return LFK_NAME_(sumo_)(ws->k7.x,n);
    case 8:
        return LFK_NAME_(sumo_)(ws->k8.u1[0][0],10*n)+
               LFK_NAME_(sumo_)(ws->k8.u2[0][0],10*n)+
               LFK_NAME_(sumo_)(ws->k8.u3[0][0],10*n);
    case 9: return LFK_NAME_(sumo_)(ws->k9.px[0],15*n);
    /* px[k][j] is laid out with j contiguous, exactly the Fortran PX(j,k)
       storage sequence. Thus this is SUMO(PX,15*n), not a logical 15-column
       gather for every active k. */
    case 10:return LFK_NAME_(sumo_)(ws->k10.px[0],15*n);
    case 11:return LFK_NAME_(sumo_)(ws->k11.x+1,n-1);
    case 12:return LFK_NAME_(sumo_)(ws->k12.x,n-1);
    /* p[k][j] keeps Fortran P(j,k) contiguous in j. h[j][i] keeps Fortran
       H(i,j) contiguous in i, so each pointer traverses its physical words. */
    case 13:return LFK_NAME_(sumo_)(ws->k13.p[0],8*n)+
                   LFK_NAME_(sumo_)(ws->k13.h[0],8*n);
    case 14:return LFK_NAME_(sumo_)(ws->k14.vx,n)+
                   LFK_NAME_(sumo_)(ws->k14.xx,n)+
                   LFK_NAME_(sumo_)(ws->k14.rh,67);
    }
    return 0.0;
}

#undef LFK_NAME_
#undef LFK_CAT_
#undef LFK_CAT2_
