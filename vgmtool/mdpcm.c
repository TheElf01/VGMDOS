#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "vgmlib.h"
#include "ym3438.h"
#include "opl3.h"
#include "mdconv.h"

extern int g_en;

#define RF_CLOCK   12500001UL
#define RF_RATE    (12500000.0 / 384.0)
#define RING       0x2000L
#define RING_DATA  (RING - 1)
#define CHUNK      2048L
#define NCH        8
#define FS         (7670454.0 / 144.0)
#define LOOP_MIN   1024L
#define GAIN       1.0

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "Out of memory\n"); exit(2); }
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "Out of memory\n"); exit(2); }
    return q;
}

static unsigned char sm_byte(int v)
{
    if (v >= 0) return (unsigned char)(v > 127 ? 127 : v);
    v = -v;
    if (v > 126) v = 126;
    return (unsigned char)(0x80 | v);
}

#define NPR 30
typedef struct {
    unsigned char pr[NPR];
    unsigned char lfo;
    int band;
    double freq;
    double cdb;
    long off, len, loop;
    double peak;
    double tail;
    double rel;
    double ltime;
    double srate;
    int pcm;
    double score;
    long uses;
    double secs;
} Smp;
static Smp *g_smp; static int g_nsmp, g_capsmp;
static Buf g_bank;
static long g_zero = 0;

static const int g_carriers[8] = { 0x8, 0x8, 0x8, 0x8, 0xA, 0xE, 0xE, 0xF };

static double carrier_db(const unsigned char *pr)
{
    int alg = pr[28] & 7, k, n = 0;
    double s = 0;
    static const int slot_of_op[4] = { 0, 2, 1, 3 };
    for (k = 0; k < 4; k++) {
        if (g_carriers[alg] & (1 << k)) {
            int tl = pr[slot_of_op[k] * 7 + 1] & 0x7F;
            s += tl * 0.75; n++;
        }
    }
    return n ? s / n : 0;
}

static void opn_w(ym3438_t *c, int port, int reg, int val)
{
    Bit16s b[2];
    int i;
    OPN2_Write(c, (Bit32u)(port * 2), (Bit8u)reg);
    for (i = 0; i < 12; i++) OPN2_Clock(c, b);
    OPN2_Write(c, (Bit32u)(port * 2 + 1), (Bit8u)val);
    for (i = 0; i < 32; i++) OPN2_Clock(c, b);
}

static int opn_sample(ym3438_t *c)
{
    Bit16s b[2];
    int i, s = 0;
    for (i = 0; i < 24; i++) { OPN2_Clock(c, b); s += b[0] + b[1]; }
    return s;
}

static void opn_setup(ym3438_t *c, const Smp *s, int fhi, int flo)
{
    static const int base[7] = { 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90 };
    int r, sl;
    OPN2_Reset(c);
    opn_w(c, 0, 0x22, s->lfo);
    opn_w(c, 0, 0x27, 0);
    opn_w(c, 0, 0x2B, 0);
    for (sl = 0; sl < 4; sl++)
        for (r = 0; r < 7; r++) opn_w(c, 0, base[r] + sl * 4, s->pr[sl * 7 + r]);
    opn_w(c, 0, 0xB0, s->pr[28]);
    opn_w(c, 0, 0xB4, 0xC0 | (s->pr[29] & 0x37));
    opn_w(c, 0, 0xA4, fhi);
    opn_w(c, 0, 0xA0, flo);
}

static double rms_db(const double *x, long n)
{
    double e = 0;
    long k;
    for (k = 0; k < n; k++) e += x[k] * x[k];
    e = n ? e / n : 0;
    return 10.0 * log10(e + 1e-9);
}

static double note_freq(int fhi, int flo)
{
    int block = (fhi >> 3) & 7, fnum = ((fhi & 7) << 8) | flo;
    return fnum * pow(2.0, block - 1) * 7670454.0 / 144.0 / 1048576.0;
}

static void lowpass(const double *x, long n, int d, double **out, long *nout)
{
    double h[49], sum = 0, *y;
    long m = n / d, k;
    int tp;
    for (tp = -24; tp <= 24; tp++) {
        double xx = tp * 0.9 / d, w = 0.5 + 0.5 * cos(3.14159265 * tp / 25.0);
        h[tp + 24] = (tp ? sin(3.14159265 * xx) / (3.14159265 * xx) : 1.0) * w;
        sum += h[tp + 24];
    }
    y = (double *)xmalloc(sizeof(double) * (size_t)(m + 1));
    for (k = 0; k < m; k++) {
        double acc = 0;
        long c0 = k * d;
        for (tp = -24; tp <= 24; tp++) {
            long q = c0 + tp;
            if (q < 0) q = 0;
            if (q >= n) q = n - 1;
            acc += h[tp + 24] * x[q];
        }
        y[k] = acc / sum;
    }
    *out = y; *nout = m;
}

typedef struct { long t; int ch, pcm; } Mode;
static Mode *g_mode; static long g_nmode, g_capmode;
static int g_npcm_notes;
static void g_mode_note(int ch, long t, int pcm)
{
    if (g_nmode >= g_capmode) {
        g_capmode = g_capmode ? g_capmode * 2 : 1024;
        g_mode = (Mode *)xrealloc(g_mode, sizeof(Mode) * (size_t)g_capmode);
    }
    g_mode[g_nmode].t = t; g_mode[g_nmode].ch = ch; g_mode[g_nmode].pcm = pcm;
    g_nmode++;
    if (pcm) g_npcm_notes++;
}
int g_md_maxch = 0;
static int g_need_score = 0;
int g_md_mix = 1;
double g_md_thr = 10.0;

static void fft_r(double *re, double *im, int n)
{
    int i, j, k, m;
    for (i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (m = 2; m <= n; m <<= 1) {
        double a = -2.0 * 3.14159265358979 / m;
        for (k = 0; k < n; k += m)
            for (j = 0; j < m / 2; j++) {
                double wr = cos(a * j), wi = sin(a * j);
                double xr = re[k + j + m / 2] * wr - im[k + j + m / 2] * wi;
                double xi = re[k + j + m / 2] * wi + im[k + j + m / 2] * wr;
                re[k + j + m / 2] = re[k + j] - xr; im[k + j + m / 2] = im[k + j] - xi;
                re[k + j] += xr; im[k + j] += xi;
            }
    }
}

#define NB 21

static void sig_profile(const double *x, long n, double fs, double *band, double *env)
{
    static double re[1024], im[1024];
    double acc[NB];
    long k, st = (long)(fs * 0.03), en = (long)(fs * 0.4);
    int b, i, nw = 0;
    for (b = 0; b < NB; b++) acc[b] = 0;
    for (k = st; k + 1024 <= en && k + 1024 <= n; k += 512) {
        for (i = 0; i < 1024; i++) { re[i] = x[k + i] * (0.5 - 0.5 * cos(6.283185 * i / 1023)); im[i] = 0; }
        fft_r(re, im, 1024);
        for (i = 1; i < 512; i++) {
            double f = i * fs / 1024, p = re[i] * re[i] + im[i] * im[i];
            int bb = (int)floor(3.0 * log(f / 80.0) / log(2.0));
            if (bb >= 0 && bb < NB) acc[bb] += p;
        }
        nw++;
    }
    for (b = 0; b < NB; b++) band[b] = 10.0 * log10(acc[b] / (nw ? nw : 1) + 1e-6);
    for (i = 0; i < 25; i++) {
        long a = (long)(fs * 0.02 * i), w = (long)(fs * 0.02), q;
        double e = 0;
        for (q = 0; q < w && a + q < n; q++) e += x[a + q] * x[a + q];
        env[i] = 10.0 * log10(e / w + 1e-6);
    }
}

static double g_deficit, g_lvl; static int g_use_def = 1;
static double prof_dist(const double *b1, const double *e1, const double *b2, const double *e2)
{
    double m1 = -999, m2 = -999, d = 0, de = 0;
    int b, nb = 0, ne = 0;
    for (b = 0; b < NB; b++) { if (b1[b] > m1) m1 = b1[b]; if (b2[b] > m2) m2 = b2[b]; }
    for (b = 0; b < NB; b++) {
        double x = b1[b] - m1, y = b2[b] - m2;
        if (x < -40 && y < -40) continue;
        if (x < -40) x = -40;
        if (y < -40) y = -40;
        d += fabs(x - y); nb++;
    }
    m1 = m2 = -999;
    for (b = 0; b < 25; b++) { if (e1[b] > m1) m1 = e1[b]; if (e2[b] > m2) m2 = e2[b]; }
    for (b = 0; b < 25; b++) {
        double x = e1[b] - m1, y = e2[b] - m2;
        if (x < -40 && y < -40) continue;
        if (x < -40) x = -40;
        if (y < -40) y = -40;
        de += fabs(x - y); ne++;
    }
    {
        double mm1 = -999, mm2 = -999, def = 0;
        for (b = 0; b < NB; b++) { if (b1[b] > mm1) mm1 = b1[b]; if (b2[b] > mm2) mm2 = b2[b]; }
        for (b = 0; b < NB; b++) {
            double x = b1[b] - mm1, y = b2[b] - mm2;
            if (y < -40) continue;
            if (x < -40) x = -40;
            if (y - x > 8.0) def += y - x - 8.0;
        }
        g_deficit = def / 6.0;
    }
    return (nb ? d / nb : 0) + 0.5 * (ne ? de / ne : 0) + (g_use_def ? g_deficit : 0);
}

static unsigned g_orhold = 22050, g_ortail = 4410;
static long opl_render(const Smp *s, int fhi, int flo, double *out, long maxn)
{
    static const int base[7] = { 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90 };
    Buf v, r;
    char msg[256];
    int sl, rg;
    long n = 0, i;
    static opl3_chip chip;
    buf_init(&v); buf_init(&r);
    buf_reserve(&v, 0x40);
    memset(v.p, 0, 0x40); v.n = 0x40;
    memcpy(v.p, "Vgm ", 4);
    wr32(&v, 0x08, 0x150);
    wr32(&v, 0x0C, 3579545UL);
    wr32(&v, 0x2C, 7670453UL);
    wr32(&v, 0x34, 0x0C);
#define YW(a, b) do { buf_byte(&v, 0x52); buf_byte(&v, (a)); buf_byte(&v, (b)); } while (0)
    YW(0x22, s->lfo); YW(0x27, 0); YW(0x2B, 0);
    for (sl = 0; sl < 4; sl++) for (rg = 0; rg < 7; rg++) YW(base[rg] + sl * 4, s->pr[sl * 7 + rg]);
    YW(0xB0, s->pr[28]); YW(0xB4, 0xC0 | (s->pr[29] & 0x37));
    YW(0xA4, fhi); YW(0xA0, flo);
    YW(0x28, 0xF0);
    buf_byte(&v, 0x61); buf_u16(&v, g_orhold);
    YW(0x28, 0x00);
    buf_byte(&v, 0x61); buf_u16(&v, g_ortail);
    buf_byte(&v, 0x66);
#undef YW
    wr32(&v, 0x04, (unsigned long)(v.n - 4));
    wr32(&v, 0x18, (unsigned long)g_orhold + g_ortail);
    if (md_convertir(&v, &r, msg) != 1) { buf_free(&v); buf_free(&r); return 0; }
    OPL3_Reset(&chip, 49716);
    {
        long doff = (rd32(&r, 8) >= 0x150 && rd32(&r, 0x34)) ? 0x34 + (long)rd32(&r, 0x34) : 0x40;
        double acc = 0;
        for (i = doff; i < r.n && n < maxn; ) {
            int c = r.p[i];
            long w = 0;
            if (c == 0x66) break;
            if (c == 0x67) { i += 7 + (long)(rd32(&r, i + 3) & 0x7FFFFFFFUL); continue; }
            if (c == 0x5E || c == 0x5F) OPL3_WriteReg(&chip, (uint16_t)(((c == 0x5F) ? 0x100 : 0) | r.p[i + 1]), r.p[i + 2]);
            else if (c == 0x61) w = r.p[i + 1] | (r.p[i + 2] << 8);
            else if (c == 0x62) w = 735;
            else if (c == 0x63) w = 882;
            else if (c >= 0x70 && c <= 0x7F) w = (c & 15) + 1;
            if (w) {
                acc += w * 49716.0 / 44100.0;
                while (acc >= 1 && n < maxn) {
                    int16_t b[2];
                    OPL3_Generate(&chip, b);
                    out[n++] = (double)b[0] + b[1];
                    acc -= 1;
                }
            }
            i += lz_largo(r.p, r.n, i);
        }
    }
    buf_free(&v); buf_free(&r);
    return n;
}

static int g_ssg_used(const unsigned char *pr)
{
    int sl;
    for (sl = 0; sl < 4; sl++) if (pr[sl * 7 + 6] & 0x08) return 1;
    return 0;
}

static double opl_vs_ym(const Smp *s, int fhi, int flo)
{
    static ym3438_t chip;
    double *a, *b, b1[NB], e1[25], b2[NB], e2[25], d;
    long na, nb = (long)(FS * 0.5), k;
    a = (double *)xmalloc(sizeof(double) * 30000);
    b = (double *)xmalloc(sizeof(double) * (size_t)(nb + 16));
    na = opl_render(s, fhi, flo, a, 49716L / 2);
    OPN2_SetChipType(0);
    opn_setup(&chip, s, fhi, flo);
    opn_w(&chip, 0, 0x28, 0xF0);
    for (k = 0; k < nb; k++) b[k] = opn_sample(&chip);
    if (na < 1000) { free(a); free(b); return 99; }
    sig_profile(a, na, 49716.0, b1, e1);
    sig_profile(b, nb, FS, b2, e2);
    {
        double ea = 0, eb = 0;
        long q0 = (long)(49716.0 * 0.03), q1 = (long)(49716.0 * 0.4), r0 = (long)(FS * 0.03), r1 = (long)(FS * 0.4);
        for (k = q0; k < q1 && k < na; k++) ea += a[k] * a[k];
        for (k = r0; k < r1 && k < nb; k++) eb += b[k] * b[k];
        g_lvl = 10.0 * log10((ea / (q1 - q0) + 1e-9) / (eb / (r1 - r0) + 1e-9));
    }
    d = prof_dist(b1, e1, b2, e2);
    free(a); free(b);
    return d;
}

static int get_smp(const unsigned char *pr, int lfo, int fhi, int flo)
{
    static ym3438_t chip;
    double f = note_freq(fhi, flo), *x, *y, peak = 0;
    int band, i, sl;
    long n, k, loopi = -1, nn;
    Smp *s;
    double tail = 0, rel = 200, srate, tail53 = 0;
    int longl = 0;
    int dec;
    if (f < 1) f = 1;
    band = (int)floor(12.0 * log(f / 440.0) / log(2.0) / 12.0 + 100.0);
    for (i = 0; i < g_nsmp; i++) {
        if (g_smp[i].band != band || g_smp[i].lfo != lfo) continue;
        for (k = 0; k < NPR; k++) {
            int sl2 = (int)(k / 7), r = (int)(k % 7);
            static const int op_of_slot[4] = { 0, 2, 1, 3 };
            if (k < 28 && r == 1) {
                if (g_carriers[pr[28] & 7] & (1 << op_of_slot[sl2])) continue;
                if (abs((g_smp[i].pr[k] & 0x7F) - (pr[k] & 0x7F)) <= 2) continue;
                break;
            }
            if (g_smp[i].pr[k] != pr[k]) break;
        }
        if (k == NPR) return i;
    }
    if (g_nsmp >= g_capsmp) {
        g_capsmp = g_capsmp ? g_capsmp * 2 : 64;
        g_smp = (Smp *)xrealloc(g_smp, sizeof(Smp) * (size_t)g_capsmp);
    }
    s = &g_smp[g_nsmp];
    memcpy(s->pr, pr, NPR);
    {
        static const int op_of_slot[4] = { 0, 2, 1, 3 };
        int sl, mn = 127, d;
        for (sl = 0; sl < 4; sl++)
            if (g_carriers[pr[28] & 7] & (1 << op_of_slot[sl])) {
                int tl = pr[sl * 7 + 1] & 0x7F;
                if (tl < mn) mn = tl;
            }
        d = mn > 16 ? mn - 16 : 0;
        if (d)
            for (sl = 0; sl < 4; sl++)
                if (g_carriers[pr[28] & 7] & (1 << op_of_slot[sl]))
                    s->pr[sl * 7 + 1] = (unsigned char)((pr[sl * 7 + 1] & 0x80) | ((pr[sl * 7 + 1] & 0x7F) - d));
    }
    s->lfo = (unsigned char)lfo; s->band = band; s->freq = f;
    s->cdb = carrier_db(s->pr);
    s->pcm = 1; s->score = 0;
    if ((g_md_mix || g_need_score) && !(g_ssg_used(pr))) {
        s->score = opl_vs_ym(s, fhi, flo);
        if (g_md_mix && s->score < g_md_thr) s->pcm = 0;
    }
    if (!s->pcm) {
        s->off = 0; s->len = 0; s->loop = -1; s->peak = 0; s->srate = 26634;
        return g_nsmp++;
    }
    {
        double *x53, last = -999, lastsl = 99;
        int still = 0, dd;
        long n53, w = 1065, mx = (long)(FS * 0.5), mxl = (long)(FS * 1.1);
        tail53 = 0;
        x53 = (double *)xmalloc(sizeof(double) * (size_t)(mxl + 16));
        OPN2_SetChipType(0);
        opn_setup(&chip, s, fhi, flo);
        opn_w(&chip, 0, 0x28, 0xF0);
        for (n53 = 0; n53 < mx; ) {
            for (k = 0; k < w && n53 < mx; k++, n53++) x53[n53] = opn_sample(&chip);
            if (n53 >= (long)(FS * 0.06)) {
                double db = rms_db(x53 + n53 - w, w), sl = db - last;
                if (db < -20) break;
                if (last > -999 && fabs(sl - lastsl) < 0.2 && sl < 0.3) {
                    if (++still >= 4) { tail53 = sl / 0.02; break; }
                }
                else still = 0;
                lastsl = (last > -999) ? sl : 99;
                last = db;
            }
        }
        longl = 0;
        if (n53 > w) {
            double d0 = rms_db(x53 + n53 - w, w), tb[1065], mn = d0, d025 = d0, db;
            long m, q, nw = (long)(FS * 1.0) / w;
            int fl = 0;
            for (m = 0; m < nw; m++) {
                for (q = 0; q < w; q++) tb[q] = opn_sample(&chip);
                db = rms_db(tb, w);
                if (m == (long)(FS * 0.25) / w) d025 = db;
                if (db < mn) mn = db;
                if (db > mn + 6.0 && db > -10) fl = 1;
            }
            if (fl && n53 < mxl) {
                longl = 1;
                opn_setup(&chip, s, fhi, flo);
                opn_w(&chip, 0, 0x28, 0xF0);
                for (n53 = 0; n53 < mxl; n53++) x53[n53] = opn_sample(&chip);
                tail53 = 0;
            } else if (d025 < d0 - 0.5) tail53 = (d025 - d0) / 0.25;
            else tail53 = 0;
        }
        {
            static const int dc[3] = { 6, 4, 3 };
            double e2, ed;
            long j;
            lowpass(x53, n53, 2, &x, &n);
            e2 = 0;
            for (j = 0; j < n; j++) e2 += x[j] * x[j];
            e2 *= 2;
            free(x);
            dec = 2;
            for (dd = 0; dd < 3; dd++) {
                lowpass(x53, n53, dc[dd], &x, &n);
                ed = 0;
                for (j = 0; j < n; j++) ed += x[j] * x[j];
                ed *= dc[dd];
                free(x);
                if (e2 - ed < e2 * 1e-3) { dec = dc[dd]; break; }
            }
            lowpass(x53, n53, dec, &x, &n);
        }
        free(x53);
        srate = FS / dec;
    }
    tail = tail53 < -0.5 ? tail53 : 0;
    if (longl && n > (long)(srate * 0.4)) {
        long N = 1024, L, Lmin = (long)(srate * 0.1), Lmax = (long)(srate * 0.7), best = -1, j;
        double be = 1e300, en = 0;
        if (Lmax > n - N - 1) Lmax = n - N - 1;
        for (j = 0; j < N; j++) en += x[n - N + j] * x[n - N + j];
        for (L = Lmin; L <= Lmax; L += 2) {
            double e = 0;
            for (j = 0; j < N && e < be; j += 4) {
                double u = x[n - N - L + j] - x[n - N + j];
                e += u * u;
            }
            if (e < be) { be = e; best = L; }
        }
        if (best > 0) {
            long b0 = best, d;
            be = 1e300;
            for (d = -2; d <= 2; d++) {
                double e = 0;
                L = b0 + d;
                if (L < 16 || L + N >= n) continue;
                for (j = 0; j < N; j++) { double u = x[n - N - L + j] - x[n - N + j]; e += u * u; }
                if (e < be) { be = e; best = L; }
            }
            loopi = n - best;
        }
        (void)en;
    }
    if (loopi < 0 && n > 512 && rms_db(x + n - 256, 256) > -20) {
        double per = srate / f;
        long kp = (long)ceil(256.0 / per), L, best, d, kk, kbest = 0;
        double be = 1e300, e0 = 1e300;
        if (kp * per > n - 256) kp = (long)floor((n - 256) / per);
        if (kp < 1) kp = 1;
        best = (long)floor(kp * per + 0.5);
        for (kk = 0; kk < 8; kk++) {
            long k2 = kp + kk;
            double bk = 1e300;
            long bl = 0;
            if (k2 * per > n - 256) break;
            L = (long)floor(k2 * per + 0.5);
            for (d = -4; d <= 4; d++) {
                long LL = L + d, j;
                double e = 0;
                if (LL < 16 || LL + 256 > n) continue;
                for (j = 0; j < 256; j++) {
                    double u = x[n - LL - 256 + j] - x[n - 256 + j];
                    e += u * u;
                }
                if (e < bk) { bk = e; bl = LL; }
            }
            if (bl == 0) continue;
            if (kk == 0) { e0 = bk; be = bk; best = bl; kbest = 0; }
            else if (bk < be && bk < e0 * 0.25) { be = bk; best = bl; kbest = kk; }
        }
        (void)kbest;
        loopi = n - best;
    }
    {
        long w = (long)(FS * 0.002), m, q;
        double buf[128], d0, d1;
        opn_setup(&chip, s, fhi, flo);
        opn_w(&chip, 0, 0x28, 0xF0);
        for (m = 0; m < (long)(FS * 0.08) - w; m++) opn_sample(&chip);
        for (q = 0; q < w; q++) buf[q] = opn_sample(&chip);
        d0 = rms_db(buf, w);
        opn_w(&chip, 0, 0x28, 0x00);
        rel = 400;
        for (m = 1; m <= 1000; m++) {
            for (q = 0; q < w; q++) buf[q] = opn_sample(&chip);
            d1 = rms_db(buf, w);
            if (d0 - d1 >= 25 || d1 < -10) { rel = (d0 - d1) / (m * 0.002); break; }
        }
        if (m > 1000) rel = (d0 - d1) / 2.0;
        if (rel < 3) rel = 3;
    }
    nn = n;
    if (loopi >= 0) {
        long ll = n - loopi, rep = (LOOP_MIN + ll - 1) / ll;
        if (rep > 1) {
            x = (double *)xrealloc(x, sizeof(double) * (size_t)(n + ll * rep + 16));
            for (k = 1; k < rep; k++) { memcpy(x + nn, x + loopi, sizeof(double) * (size_t)ll); nn += ll; }
        }
    }
    y = x;
    for (k = 0; k < nn; k++) if (fabs(y[k]) > peak) peak = fabs(y[k]);
    s->off = g_bank.n; s->len = nn; s->loop = loopi; s->peak = peak;
    s->uses = 0; s->secs = 0;
    s->tail = tail; s->rel = rel; s->srate = srate;
    s->ltime = (loopi >= 0 ? loopi : nn) / srate;
    for (k = 0; k < nn; k++) {
        int q = peak > 0 ? (int)floor(y[k] * 127.0 / peak + 0.5) : 0;
        buf_byte(&g_bank, sm_byte(q));
    }
    for (k = 0; k < 64; k++) buf_byte(&g_bank, 0);
    free(x);
    (void)sl;
    return g_nsmp++;
}

typedef struct { long t; long seq; long off; int len; } Ev;
static Ev *g_ev; static long g_nev, g_capev, g_seq;
static Buf g_evdata;

static void emit(long t, const unsigned char *b, int len)
{
    if (g_nev >= g_capev) {
        g_capev = g_capev ? g_capev * 2 : 4096;
        g_ev = (Ev *)xrealloc(g_ev, sizeof(Ev) * (size_t)g_capev);
    }
    g_ev[g_nev].t = t; g_ev[g_nev].seq = g_seq++;
    g_ev[g_nev].off = g_evdata.n; g_ev[g_nev].len = len;
    buf_add(&g_evdata, b, len);
    g_nev++;
}

static void rf_reg(long t, int r, int v)
{
    unsigned char x[3];
    x[0] = 0xB1; x[1] = (unsigned char)r; x[2] = (unsigned char)v;
    emit(t, x, 3);
}

static void rf_copy(long t, int wbank, long src, long dst, long n)
{
    unsigned char x[12];
    rf_reg(t, 7, 0x80 | wbank);
    x[0] = 0x68; x[1] = 0x66; x[2] = 0x02;
    x[3] = (unsigned char)src; x[4] = (unsigned char)(src >> 8); x[5] = (unsigned char)(src >> 16);
    x[6] = (unsigned char)dst; x[7] = (unsigned char)(dst >> 8); x[8] = (unsigned char)(dst >> 16);
    x[9] = (unsigned char)n; x[10] = (unsigned char)(n >> 8); x[11] = (unsigned char)(n >> 16);
    emit(t, x, 12);
}

static int cmp_ev(const void *a, const void *b)
{
    const Ev *x = (const Ev *)a, *y = (const Ev *)b;
    if (x->t != y->t) return x->t < y->t ? -1 : 1;
    return (x->seq > y->seq) - (x->seq < y->seq);
}

typedef struct {
    int on, smp;
    double rate, pos0;
    long t0, written, tend;
    int env, pan;
    unsigned fd;
    long tenv, tfd, ts;
} Ch;
static Ch g_ch[NCH];
static unsigned g_mask = 0xFF;

static double ch_pos(const Ch *c, long t) { return c->pos0 + (double)(t - c->t0) * c->rate / 44100.0; }

static unsigned rate_fd(double rate)
{
    double f = rate / RF_RATE * 2048.0;
    if (f < 1) f = 1;
    if (f > 65535) f = 65535;
    return (unsigned)floor(f + 0.5);
}

static long ch_lead(const Ch *c)
{
    long l = (long)(c->rate * 0.1);
    if (l < CHUNK) l = CHUNK;
    if (l > RING_DATA / 2) l = RING_DATA / 2;
    return l;
}

static long ch_total(const Ch *c)
{
    const Smp *s = &g_smp[c->smp];
    return s->loop >= 0 ? 0x7FFFFFFFL : s->len + 256;
}

static void ch_write(int k, long t, long b, long n)
{
    Ch *c = &g_ch[k];
    const Smp *s = &g_smp[c->smp];
    while (n > 0) {
        long si, rp, m, lim;
        if (s->loop >= 0) {
            si = b < s->len ? b : s->loop + (b - s->loop) % (s->len - s->loop);
            lim = s->len - si;
        } else {
            if (b >= s->len + 256) return;
            si = b;
            lim = si < s->len ? s->len - si : s->len + 256 - si;
        }
        rp = b % RING_DATA;
        m = n;
        if (m > RING_DATA - rp) m = RING_DATA - rp;
        if (m > lim) m = lim;
        if (s->loop < 0 && si >= s->len) rf_copy(t, k * 2, g_zero + (si - s->len), rp, m);
        else rf_copy(t, k * 2, s->off + si, rp, m);
        b += m; n -= m;
    }
}

static void ch_settend(Ch *c)
{
    const Smp *s = &g_smp[c->smp];
    if (s->loop >= 0) c->tend = 0x7FFFFFFFL;
    else c->tend = c->t0 + (long)ceil(((double)s->len - c->pos0) * 44100.0 / c->rate) + 1;
}

static void flush_until(long t)
{
    int k;
    for (k = 0; k < NCH; k++) {
        Ch *c = &g_ch[k];
        long total;
        if (!c->on) continue;
        total = ch_total(c);
        for (;;) {
            long b = c->written, tw;
            double need;
            if (b >= total) break;
            need = (double)(b - ch_lead(c));
            tw = c->t0 + (long)ceil((need - c->pos0) * 44100.0 / c->rate);
            tw = tw / 220 * 220;
            if (tw < c->t0) tw = c->t0;
            if (tw > t || tw >= c->tend) break;
            ch_write(k, tw, b, CHUNK);
            c->written += CHUNK;
        }
        if (c->tend <= t) {
            g_smp[c->smp].secs += (double)(c->tend - c->ts) / 44100.0;
            g_mask |= 1u << k;
            rf_reg(c->tend, 8, (int)g_mask);
            c->on = 0;
        }
    }
}

static void ch_start(int k, long t, int smp, double rate, int env, int pan)
{
    Ch *c = &g_ch[k];
    long first;
    g_mask |= 1u << k;
    rf_reg(t, 8, (int)g_mask);
    if (c->on && t > c->ts) g_smp[c->smp].secs += (double)(t - c->ts) / 44100.0;
    c->on = 1; c->smp = smp;
    c->fd = rate_fd(rate);
    c->rate = c->fd * RF_RATE / 2048.0;
    c->pos0 = 0; c->t0 = t; c->written = 0;
    c->env = env; c->pan = pan; c->tenv = c->tfd = t;
    c->ts = t; g_smp[smp].uses++;
    ch_settend(c);
    first = ch_lead(c) + CHUNK;
    first = (first + CHUNK - 1) / CHUNK * CHUNK;
    if (first > ch_total(c)) first = ch_total(c);
    ch_write(k, t, 0, first);
    c->written = first;
    rf_reg(t, 7, 0xC0 | k);
    rf_reg(t, 0, c->env);
    rf_reg(t, 1, c->pan);
    rf_reg(t, 2, c->fd & 0xFF);
    rf_reg(t, 3, c->fd >> 8);
    rf_reg(t, 4, (int)((k * RING) & 0xFF));
    rf_reg(t, 5, (int)(((k * RING) >> 8) & 0xFF));
    rf_reg(t, 6, (int)((k * RING) >> 8));
    g_mask &= ~(1u << k);
    rf_reg(t, 8, (int)g_mask);
}

static void rec_stop(int k, long t);
static void ch_stop(int k, long t)
{
    if (!g_ch[k].on) return;
    rec_stop(k, t);
    if (t > g_ch[k].ts) g_smp[g_ch[k].smp].secs += (double)(t - g_ch[k].ts) / 44100.0;
    g_ch[k].on = 0;
    g_mask |= 1u << k;
    rf_reg(t, 8, (int)g_mask);
}

static void ch_setfd(int k, long t, unsigned fd)
{
    Ch *c = &g_ch[k];
    c->pos0 = ch_pos(c, t); c->t0 = t;
    c->fd = fd; c->rate = fd * RF_RATE / 2048.0;
    c->tfd = t;
    ch_settend(c);
    rf_reg(t, 7, 0xC0 | k);
    rf_reg(t, 2, fd & 0xFF);
    rf_reg(t, 3, fd >> 8);
}

static void ch_env(int k, long t, int env, int pan)
{
    Ch *c = &g_ch[k];
    if (env == c->env && pan == c->pan) return;
    rf_reg(t, 7, 0xC0 | k);
    if (env != c->env) rf_reg(t, 0, env);
    if (pan != c->pan) rf_reg(t, 1, pan);
    c->env = env; c->pan = pan; c->tenv = t;
}

typedef struct {
    int keyon;
    int released;
    long ton, toff;
    double gdb;
    double rate0;
} Note;

static unsigned char g_reg[2][256];
static Note g_note[6];
static int g_dac_on = 0;

static void chan_pr(int ch, unsigned char *pr)
{
    int port = ch / 3, c = ch % 3, sl, r;
    static const int base[7] = { 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90 };
    for (sl = 0; sl < 4; sl++)
        for (r = 0; r < 7; r++) pr[sl * 7 + r] = g_reg[port][base[r] + sl * 4 + c];
    pr[28] = g_reg[port][0xB0 + c];
    pr[29] = g_reg[port][0xB4 + c] & 0x37;
}

static int pan_of(int ch)
{
    int b4 = g_reg[ch / 3][0xB4 + ch % 3], p = 0;
    if (b4 & 0x80) p |= 0x0F;
    if (b4 & 0x40) p |= 0xF0;
    return p;
}

static double fm_freq(int ch)
{
    return note_freq(g_reg[ch / 3][0xA4 + ch % 3], g_reg[ch / 3][0xA0 + ch % 3]);
}

static int note_env(int ch, long t)
{
    Note *nt = &g_note[ch];
    Ch *c = &g_ch[ch];
    const Smp *s;
    unsigned char pr[NPR];
    double db, a;
    if (!c->on) return 0;
    s = &g_smp[c->smp];
    chan_pr(ch, pr);
    db = s->cdb - carrier_db(pr);
    if (s->tail < 0) {
        double since = (t - nt->ton) / 44100.0 - s->ltime;
        if (since > 0) db += s->tail * since;
    }
    if (nt->released) db -= s->rel * (t - nt->toff) / 44100.0;
    a = pow(10.0, db / 20.0);
    a = 255.0 * GAIN * a * s->peak / 2048.0;
    if (a > 255) a = 255;
    return (int)floor(a + 0.5);
}

static void limit_voices(long t)
{
    if (g_md_maxch <= 0) return;
    for (;;) {
        int k, n = 0, w = -1;
        for (k = 0; k < 6; k++)
            if (g_ch[k].on && g_note[k].keyon) {
                n++;
                if (w < 0 || g_ch[k].env < g_ch[w].env) w = k;
            }
        if (n <= g_md_maxch) break;
        ch_stop(w, t);
        g_note[w].keyon = 0;
    }
}

static long g_n_steal = 0, g_n_fall = 0;
typedef struct { long t, tend; int ch; double prio; unsigned char pcm; } NRec;
static NRec *g_nrec_a; static long g_nrec, g_nrec_cap, g_rpos;
static int g_pass = 0, g_recopen[6];

static void rec_stop(int k, long t)
{
    if (g_pass == 1 && g_recopen[k] >= 0) {
        if (g_nrec_a[g_recopen[k]].tend < 0) g_nrec_a[g_recopen[k]].tend = t;
        g_recopen[k] = -1;
    }
}
static double prio_smp(const Smp *s) { return g_ssg_used(s->pr) ? 99.0 : s->score; }

static void key_on(int ch, long t)
{
    unsigned char pr[NPR];
    int sm, fhi, flo;
    Note *nt = &g_note[ch];
    double rate;
    int pcm_ok;
    if (ch == 5 && g_dac_on) return;
    chan_pr(ch, pr);
    fhi = g_reg[ch / 3][0xA4 + ch % 3]; flo = g_reg[ch / 3][0xA0 + ch % 3];
    sm = get_smp(pr, g_reg[0][0x22], fhi, flo);
    pcm_ok = g_smp[sm].pcm;
    if (pcm_ok && g_pass == 1) {
        if (g_recopen[ch] >= 0 && g_nrec_a[g_recopen[ch]].tend < 0) g_nrec_a[g_recopen[ch]].tend = t;
        if (g_nrec >= g_nrec_cap) {
            g_nrec_cap = g_nrec_cap ? g_nrec_cap * 2 : 1024;
            g_nrec_a = (NRec *)xrealloc(g_nrec_a, sizeof(NRec) * (size_t)g_nrec_cap);
        }
        g_nrec_a[g_nrec].t = t; g_nrec_a[g_nrec].tend = -1; g_nrec_a[g_nrec].ch = ch;
        g_nrec_a[g_nrec].prio = prio_smp(&g_smp[sm]); g_nrec_a[g_nrec].pcm = 1;
        g_recopen[ch] = (int)g_nrec++;
    } else if (pcm_ok && g_pass == 2) {
        if (g_rpos < g_nrec && g_nrec_a[g_rpos].t == t && g_nrec_a[g_rpos].ch == ch) pcm_ok = g_nrec_a[g_rpos].pcm;
        g_rpos++;
    }
    g_mode_note(ch, t, pcm_ok);
    if (!pcm_ok) {
        ch_stop(ch, t);
        nt->keyon = 0;
        return;
    }
    rate = g_smp[sm].srate * fm_freq(ch) / g_smp[sm].freq;
    ch_start(ch, t, sm, rate, 0, pan_of(ch));
    nt->keyon = 1; nt->released = 0; nt->ton = t;
    g_ch[ch].env = -1;
    ch_env(ch, t, note_env(ch, t), pan_of(ch));
    limit_voices(t);
}

static void key_off(int ch, long t)
{
    Note *nt = &g_note[ch];
    if (!nt->keyon || nt->released) return;
    nt->released = 1; nt->toff = t;
}

static void tick(long t)
{
    int ch;
    for (ch = 0; ch < 6; ch++) {
        Ch *c = &g_ch[ch];
        Note *nt = &g_note[ch];
        int env;
        if (!c->on || !nt->keyon) continue;
        env = note_env(ch, t);
        if (nt->released && env <= 2) { ch_stop(ch, t); nt->keyon = 0; continue; }
        {
            int d0 = abs(env - c->env), lim = c->env / 6;
            if (lim < 3) lim = 3;
            if (d0 >= lim || env == 0 || (env != c->env && t - c->tenv >= 1764) || pan_of(ch) != c->pan)
                ch_env(ch, t, env, pan_of(ch));
        }
        {
            double rate = g_smp[c->smp].srate * fm_freq(ch) / g_smp[c->smp].freq;
            unsigned fd = rate_fd(rate);
            if (fd != c->fd) {
                double r = (double)fd / c->fd;
                if (r > 1.003 || r < 0.997 || t - c->tfd >= 441) ch_setfd(ch, t, fd);
            }
        }
    }
}

static void put_wait(Buf *o, long w)
{
    while (w > 0) { long q = w > 65535 ? 65535 : w; buf_byte(o, 0x61); buf_u16(o, (unsigned)q); w -= q; }
}

typedef struct { long t; int kind; long off, len; unsigned long rate; } DEv;
static DEv *g_dev; static long g_ndev, g_idev;
static Buf g_dbank;
static int g_dpan = 0xFF;
typedef struct { long off, len; unsigned long rate; int smp; } DSmp;
static DSmp *g_dsm; static int g_ndsm;

static long g_dbase = -1;
static double g_dpeak = 1;

static int get_dac_smp(long off, long len, unsigned long rate)
{
    int i;
    long k;
    Smp *s;
    for (i = 0; i < g_ndsm; i++)
        if (g_dsm[i].off == off && g_dsm[i].len == len && g_dsm[i].rate == rate) return g_dsm[i].smp;
    if (off >= g_dbank.n) return -1;
    if (off + len > g_dbank.n) len = g_dbank.n - off;
    if (len <= 0) return -1;
    if (g_dbase < 0) {
        g_dpeak = 1;
        for (k = 0; k < g_dbank.n; k++) { double v = fabs((double)g_dbank.p[k] - 128.0); if (v > g_dpeak) g_dpeak = v; }
        g_dbase = g_bank.n;
        for (k = 0; k < g_dbank.n; k++)
            buf_byte(&g_bank, sm_byte((int)floor((g_dbank.p[k] - 128.0) * 127.0 / g_dpeak + 0.5)));
        for (k = 0; k < 64; k++) buf_byte(&g_bank, 0);
    }
    if (g_nsmp >= g_capsmp) {
        g_capsmp = g_capsmp ? g_capsmp * 2 : 64;
        g_smp = (Smp *)xrealloc(g_smp, sizeof(Smp) * (size_t)g_capsmp);
    }
    s = &g_smp[g_nsmp];
    memset(s, 0, sizeof(Smp));
    s->band = -1000; s->srate = (double)rate; s->freq = 1;
    s->off = g_dbase + off; s->len = len; s->loop = -1; s->peak = g_dpeak;
    g_dsm = (DSmp *)xrealloc(g_dsm, sizeof(DSmp) * (size_t)(g_ndsm + 1));
    g_dsm[g_ndsm].off = off; g_dsm[g_ndsm].len = len; g_dsm[g_ndsm].rate = rate; g_dsm[g_ndsm].smp = g_nsmp;
    g_ndsm++;
    return g_nsmp++;
}

static void dac_parse(const Buf *conv)
{
    long doff = (rd32(conv, 8) >= 0x150 && rd32(conv, 0x34)) ? 0x34 + (long)rd32(conv, 0x34) : 0x40, i, t = 0;
    unsigned long rate = 0;
    for (i = doff; i < conv->n; ) {
        int c = conv->p[i];
        if (c == 0x66) break;
        if (c == 0x67) {
            long sz = (long)(rd32(conv, i + 3) & 0x7FFFFFFFUL);
            if (conv->p[i + 2] == 0x00) buf_add(&g_dbank, conv->p + i + 7, sz);
            i += 7 + sz; continue;
        }
        if (c == 0x61) t += conv->p[i + 1] | (conv->p[i + 2] << 8);
        else if (c == 0x62) t += 735;
        else if (c == 0x63) t += 882;
        else if (c >= 0x70 && c <= 0x7F) t += (c & 15) + 1;
        else if (c == 0x52 && conv->p[i + 1] == 0xB6) {
            int v = conv->p[i + 2];
            g_dpan = ((v & 0x80) ? 0x0F : 0) | ((v & 0x40) ? 0xF0 : 0);
        } else if (c == 0x92) rate = rd32(conv, i + 2);
        else if (c == 0x93 || c == 0x94) {
            g_dev = (DEv *)xrealloc(g_dev, sizeof(DEv) * (size_t)(g_ndev + 1));
            g_dev[g_ndev].t = t; g_dev[g_ndev].rate = rate;
            if (c == 0x94) { g_dev[g_ndev].kind = 1; g_dev[g_ndev].off = g_dev[g_ndev].len = 0; }
            else {
                int mode = conv->p[i + 6] & 0x7F;
                long off = (long)rd32(conv, i + 2), len = (long)rd32(conv, i + 7);
                if (mode == 3 || mode == 0) len = 0x7FFFFFFFL;
                else if (mode == 2) len = (long)((double)len * rate / 1000.0);
                g_dev[g_ndev].kind = 0; g_dev[g_ndev].off = off; g_dev[g_ndev].len = len;
            }
            g_ndev++;
        }
        i += lz_largo(conv->p, conv->n, i);
    }
}

typedef struct { long t; int kind; int smp; } PEv;
static PEv *g_pev; static long g_npev, g_ipev;
int g_md_pwmonly = 0;

static long pwm_scan(const Buf *orig, long doff, long **pt, int **pv, double *center)
{
    long i, t = 0, n = 0, cap = 0, n0 = orig->n;
    int curL = 0, curR = 0, cyc = 1475;
    *pt = NULL; *pv = NULL;
    for (i = doff; i < n0; ) {
        int c = orig->p[i];
        if (c == 0x66) break;
        if (c == 0x67) { i += 7 + (long)(rd32(orig, i + 3) & 0x7FFFFFFFUL); continue; }
        if (c == 0x61) t += orig->p[i + 1] | (orig->p[i + 2] << 8);
        else if (c == 0x62) t += 735;
        else if (c == 0x63) t += 882;
        else if (c >= 0x70 && c <= 0x7F) t += (c & 15) + 1;
        else if (c >= 0x80 && c <= 0x8F) t += c & 15;
        else if (c == 0xB2 && i + 2 < n0) {
            int reg = orig->p[i + 1] >> 4, val = ((orig->p[i + 1] & 15) << 8) | orig->p[i + 2], v = -1;
            if (reg == 1 && val > 0) cyc = val;
            else if (reg == 2) { curL = val; v = (curL + curR) / 2; }
            else if (reg == 3) { curR = val; v = (curL + curR) / 2; }
            else if (reg == 4) v = val;
            if (v >= 0) {
                if (n >= cap) {
                    cap = cap ? cap * 2 : 4096;
                    *pt = (long *)xrealloc(*pt, sizeof(long) * (size_t)cap);
                    *pv = (int *)xrealloc(*pv, sizeof(int) * (size_t)cap);
                }
                (*pt)[n] = t; (*pv)[n] = v; n++;
            }
        }
        i += lz_largo(orig->p, n0, i);
    }
    *center = cyc / 2.0;
    return n;
}

static void pwm_build(const Buf *orig, long doff)
{
    long *pt, nev, k, nq, kk, run, a, b, tend;
    int *pv;
    double center, rate = 14700.0, gain = 1.0, dt;
    signed char *q;
    long *dts, nd = 0;
    g_pev = NULL; g_npev = g_ipev = 0;
    nev = pwm_scan(orig, doff, &pt, &pv, &center);
    if (nev < 64) { free(pt); free(pv); return; }
    dts = (long *)xrealloc(NULL, sizeof(long) * 16);
    {
        long hist[64], best = 3, bc = 0;
        memset(hist, 0, sizeof(hist));
        for (k = 1; k < nev && k < 4000; k++) { long d = pt[k] - pt[k - 1]; if (d > 0 && d < 64) hist[d]++; }
        for (k = 1; k < 64; k++) if (hist[k] > bc) { bc = hist[k]; best = k; }
        rate = 44100.0 / best;
        if (rate > 22050.0) rate = 22050.0;
    }
    free(dts); (void)nd;
    dt = 44100.0 / rate;
    nq = (long)((double)pt[nev - 1] / dt) + 2;
    q = (signed char *)xrealloc(NULL, (size_t)nq);
    {
        long j = 0;
        for (kk = 0; kk < nq; kk++) {
            double tk = kk * dt, v;
            int qi;
            while (j + 1 < nev && pt[j + 1] <= tk) j++;
            v = (pv[j] - center) * 127.0 / 450.0 * gain;
            qi = (int)floor(v + 0.5);
            if (qi > 127) qi = 127;
            if (qi < -127) qi = -127;
            q[kk] = (signed char)qi;
        }
    }
    free(pt); free(pv);
    {
        long gap = (long)(rate * 0.12), pad = (long)(rate * 0.004);
        kk = 0;
        while (kk < nq) {
            while (kk < nq && abs(q[kk]) <= 1) kk++;
            if (kk >= nq) break;
            a = kk; run = 0; b = kk;
            while (kk < nq) {
                if (abs(q[kk]) > 1) { b = kk; run = 0; }
                else if (++run > gap) break;
                kk++;
            }
            a -= pad; b += pad + 8;
            if (a < 0) a = 0;
            if (b >= nq) b = nq - 1;
            if (b - a < 16) continue;
            if (g_nsmp >= g_capsmp) {
                g_capsmp = g_capsmp ? g_capsmp * 2 : 64;
                g_smp = (Smp *)xrealloc(g_smp, sizeof(Smp) * (size_t)g_capsmp);
            }
            {
                Smp *s = &g_smp[g_nsmp];
                long j;
                memset(s, 0, sizeof(Smp));
                s->band = -1000; s->srate = rate; s->freq = 1; s->loop = -1; s->peak = 127;
                s->off = g_bank.n; s->len = b - a + 1;
                for (j = a; j <= b; j++) buf_byte(&g_bank, sm_byte(q[j]));
                for (j = 0; j < 64; j++) buf_byte(&g_bank, 0);
                tend = (long)((b + 1) * dt);
                g_pev = (PEv *)xrealloc(g_pev, sizeof(PEv) * (size_t)(g_npev + 2));
                g_pev[g_npev].t = (long)(a * dt); g_pev[g_npev].kind = 0; g_pev[g_npev].smp = g_nsmp; g_npev++;
                g_pev[g_npev].t = tend; g_pev[g_npev].kind = 1; g_pev[g_npev].smp = g_nsmp; g_npev++;
                g_nsmp++;
            }
        }
    }
    free(q);
}

static void dac_until(long t)
{
    while (g_ipev < g_npev && g_pev[g_ipev].t <= t) {
        PEv *e = &g_pev[g_ipev++];
        flush_until(e->t);
        if (e->kind == 1) ch_stop(7, e->t);
        else ch_start(7, e->t, e->smp, g_smp[e->smp].srate, 220, 0xFF);
    }
    while (g_idev < g_ndev && g_dev[g_idev].t <= t) {
        DEv *e = &g_dev[g_idev++];
        flush_until(e->t);
        if (e->kind == 1) ch_stop(6, e->t);
        else if (e->rate > 0) {
            int sm = get_dac_smp(e->off, e->len, e->rate);
            if (sm >= 0) {
                double env = g_smp[sm].peak * 1.27;
                ch_start(6, e->t, sm, g_smp[sm].srate, env > 255 ? 255 : (int)(env + 0.5), g_dpan);
            }
        }
    }
}

static const Smp *g_awe_base;
static int awe_cmp(const void *a, const void *b)
{
    const Smp *x = &g_awe_base[*(const int *)a], *y = &g_awe_base[*(const int *)b];
    double dx = (x->secs + 0.05 * x->uses) / (double)(x->len + 64), dy = (y->secs + 0.05 * y->uses) / (double)(y->len + 64);
    return (dx < dy) - (dx > dy);
}

static void put_awe_table(Buf *o)
{
    int *ord = (int *)xrealloc(NULL, sizeof(int) * (size_t)(g_nsmp + 1)), n = 0, i;
    long sz;
    for (i = 0; i < g_nsmp; i++)
        if (g_smp[i].uses > 0 && g_smp[i].len > 0) ord[n++] = i;
    g_awe_base = g_smp;
    qsort(ord, (size_t)n, sizeof(int), awe_cmp);
    if (n > 400) n = 400;
    if (n == 0) { free(ord); return; }
    sz = 2 + 2 + (long)n * 14;
    buf_byte(o, 0x67); buf_byte(o, 0x66); buf_byte(o, 0xC0);
    buf_u32(o, (unsigned long)sz);
    buf_byte(o, 0xFE); buf_byte(o, 0xFF);
    buf_byte(o, n & 255); buf_byte(o, n >> 8);
    for (i = 0; i < n; i++) {
        const Smp *s = &g_smp[ord[i]];
        long w = (long)((s->secs + 0.05 * s->uses) * 10.0 + 0.5);
        if (w < 1) w = 1;
        if (w > 65535) w = 65535;
        buf_u32(o, (unsigned long)s->off);
        buf_u32(o, (unsigned long)s->len);
        buf_u32(o, (unsigned long)(s->loop >= 0 ? s->loop : -1L));
        buf_byte(o, (int)(w & 255)); buf_byte(o, (int)(w >> 8));
    }
    free(ord);
}

static int mdpcm_convertir_in(const Buf *orig, const Buf *conv, Buf *res, char *msg)
{
    long doff, i, n = orig->n, t = 0, nextick = 0;
    int k;
    msg[0] = 0;
    if (conv->n < 0x40 || rd32(conv, 0x6C)) return 0;
    doff = (rd32(orig, 8) >= 0x150 && rd32(orig, 0x34)) ? 0x34 + (long)rd32(orig, 0x34) : 0x40;
    buf_init(&g_bank); buf_init(&g_evdata);
    { int z; g_zero = 0; for (z = 0; z < 256; z++) buf_byte(&g_bank, 0); }
    g_smp = NULL; g_nsmp = g_capsmp = 0; g_ev = NULL; g_nev = g_capev = g_seq = 0;
    memset(g_ch, 0, sizeof(g_ch)); memset(g_note, 0, sizeof(g_note)); memset(g_reg, 0, sizeof(g_reg));
    g_mask = 0xFF; g_dac_on = 0;
    for (k = 0; k < 6; k++) g_reg[k / 3][0xB4 + k % 3] = 0xC0;
    g_mode = NULL; g_nmode = g_capmode = 0; g_npcm_notes = 0; g_n_steal = 0;
    buf_init(&g_dbank); g_dev = NULL; g_ndev = g_idev = 0; g_dsm = NULL; g_ndsm = 0; g_dpan = 0xFF; g_dbase = -1;
    dac_parse(conv);
    pwm_build(orig, doff);
    rf_reg(0, 7, 0x80);
    rf_reg(0, 8, 0xFF);
    for (k = 0; k < NCH; k++) {
        unsigned char x[4];
        rf_reg(0, 7, 0x80 | (k * 2 + 1));
        x[0] = 0xC2; x[1] = 0xFF; x[2] = 0x0F; x[3] = 0xFF;
        emit(0, x, 4);
    }
    for (i = doff; i < n; ) {
        int c = orig->p[i];
        long w = 0;
        if (c == 0x66) break;
        if (c == 0x67) { i += 7 + (long)(rd32(orig, i + 3) & 0x7FFFFFFFUL); continue; }
        if (c == 0x61) w = orig->p[i + 1] | (orig->p[i + 2] << 8);
        else if (c == 0x62) w = 735;
        else if (c == 0x63) w = 882;
        else if (c >= 0x70 && c <= 0x7F) w = (c & 15) + 1;
        else if (c >= 0x80 && c <= 0x8F) w = c & 15;
        else if ((c == 0x52 || c == 0x53) && i + 2 < n) {
            int port = c - 0x52, reg = orig->p[i + 1], val = orig->p[i + 2];
            g_reg[port][reg] = (unsigned char)val;
            if (port == 0 && reg == 0x28) {
                int ch = (val & 3) + ((val & 4) ? 3 : 0);
                if ((val & 3) != 3 && ch < 6) {
                    dac_until(t);
                    flush_until(t);
                    if (val & 0xF0) key_on(ch, t); else key_off(ch, t);
                }
            } else if (port == 0 && reg == 0x2B) {
                g_dac_on = (val & 0x80) != 0;
                if (g_dac_on && g_note[5].keyon) { ch_stop(5, t); g_note[5].keyon = 0; }
            }
        }
        if (w) {
            long tn = t + w;
            while (nextick <= tn) {
                if (nextick > t) { dac_until(nextick); flush_until(nextick); tick(nextick); }
                nextick += 110;
            }
            t = tn;
        }
        i += lz_largo(orig->p, n, i);
    }
    dac_until(t);
    flush_until(t);
    for (k = 0; k < NCH; k++) ch_stop(k, t);
    if (g_md_mix && g_npcm_notes == 0 && g_npev == 0) {
        strcpy(msg, g_en ? "sampled FM: not needed (OPL3 is close)" : "FM grabado: no hace falta (el OPL3 se parece)");
        buf_free(&g_bank); buf_free(&g_evdata); buf_free(&g_dbank);
        free(g_smp); free(g_ev); free(g_dev); free(g_dsm); free(g_mode); free(g_pev); g_pev = NULL;
        return 0;
    }
    qsort(g_ev, (size_t)g_nev, sizeof(Ev), cmp_ev);
    {
        int last = -1;
        long j = 0;
        for (i = 0; i < g_nev; i++) {
            const unsigned char *p = g_evdata.p + g_ev[i].off;
            if (g_ev[i].len == 3 && p[0] == 0xB1 && p[1] == 7) {
                if (p[2] == last) continue;
                last = p[2];
            }
            g_ev[j++] = g_ev[i];
        }
        g_nev = j;
    }
    {
        long cdoff = (rd32(conv, 8) >= 0x150 && rd32(conv, 0x34)) ? 0x34 + (long)rd32(conv, 0x34) : 0x40;
        long loop_abs = rd32(conv, 0x1C) ? 0x1C + (long)rd32(conv, 0x1C) : -1, loop_out = -1;
        long ct = 0, cur = 0, e = 0, g, mi = 0;
        int mcur[6] = { 0, 0, 0, 0, 0, 0 };
        long hlen = cdoff < 0x100 ? cdoff : 0x100;
        Buf o;
        buf_init(&o);
        buf_reserve(&o, 0x100);
        memset(o.p, 0, 0x100); o.n = 0x100;
        memcpy(o.p, conv->p, (size_t)hlen);
        if (rd32(conv, 8) < 0x171) wr32(&o, 0x08, 0x171);
        wr32(&o, 0x34, 0x100 - 0x34);
        wr32(&o, 0x6C, RF_CLOCK);
        wr32(&o, 0x2C, 0);
        wr32(&o, 0x1C, 0); wr32(&o, 0x20, 0);
        buf_byte(&o, 0x67); buf_byte(&o, 0x66); buf_byte(&o, 0x02);
        buf_u32(&o, (unsigned long)g_bank.n);
        buf_add(&o, g_bank.p, g_bank.n);
        put_awe_table(&o);
        buf_byte(&o, 0x5F); buf_byte(&o, 0x05); buf_byte(&o, 0x01);
        buf_byte(&o, 0x70);
        for (i = cdoff; i < conv->n; ) {
            int c = conv->p[i];
            long w = 0, L;
            if (i == loop_abs) {
                while (e < g_nev && g_ev[e].t <= ct) {
                    if (g_ev[e].t > cur) { put_wait(&o, g_ev[e].t - cur); cur = g_ev[e].t; }
                    buf_add(&o, g_evdata.p + g_ev[e].off, g_ev[e].len); e++;
                }
                if (ct > cur) { put_wait(&o, ct - cur); cur = ct; }
                loop_out = o.n;
                wr32(&o, 0x20, (unsigned long)rd32(conv, 0x20));
            }
            if (c == 0x66) break;
            if (c == 0x67) {
                L = 7 + (long)(rd32(conv, i + 3) & 0x7FFFFFFFUL);
                if (conv->p[i + 2] != 0x00 && conv->p[i + 2] != 0x3F) buf_add(&o, conv->p + i, L);
                i += L; continue;
            }
            if (c == 0x61) w = conv->p[i + 1] | (conv->p[i + 2] << 8);
            else if (c == 0x62) w = 735;
            else if (c == 0x63) w = 882;
            else if (c >= 0x70 && c <= 0x7F) w = (c & 15) + 1;
            L = lz_largo(conv->p, conv->n, i);
            if (w) { ct += w; i += L; continue; }
            while (e < g_nev && g_ev[e].t <= ct) {
                if (g_ev[e].t > cur) { put_wait(&o, g_ev[e].t - cur); cur = g_ev[e].t; }
                buf_add(&o, g_evdata.p + g_ev[e].off, g_ev[e].len); e++;
            }
            if (ct > cur) { put_wait(&o, ct - cur); cur = ct; }
            if ((c == 0x5E || c == 0x5F) && conv->p[i + 1] >= 0xB0 && conv->p[i + 1] <= 0xB8) {
                static const int own[18] = { 0, 1, 2, 0, 1, 2, 0, 1, 2, 3, 4, 5, 3, 4, 5, 3, 4, 5 };
                int oc = conv->p[i + 1] - 0xB0 + (c == 0x5F ? 9 : 0), fm = own[oc], pcm = 0;
                long q;
                while (mi < g_nmode && g_mode[mi].t <= ct) { mcur[g_mode[mi].ch] = g_mode[mi].pcm; mi++; }
                pcm = mcur[fm];
                q = conv->p[i + 2];
                if (pcm) q &= ~0x20;
                buf_byte(&o, c); buf_byte(&o, conv->p[i + 1]); buf_byte(&o, (int)q);
            } else if (c != 0x52 && c != 0x53 && !(c >= 0x90 && c <= 0x95))
                buf_add(&o, conv->p + i, L);
            i += L;
        }
        while (e < g_nev) {
            if (g_ev[e].t > cur) { put_wait(&o, g_ev[e].t - cur); cur = g_ev[e].t; }
            buf_add(&o, g_evdata.p + g_ev[e].off, g_ev[e].len); e++;
        }
        if (ct > cur) { put_wait(&o, ct - cur); cur = ct; }
        buf_byte(&o, 0x66);
        if (loop_out >= 0) wr32(&o, 0x1C, (unsigned long)(loop_out - 0x1C));
        wr32(&o, 0x18, (unsigned long)cur);
        g = (long)rd32(conv, 0x14);
        if (g && 0x18 + g <= conv->n && memcmp(conv->p + 0x14 + g, "Gd3 ", 4) == 0) {
            wr32(&o, 0x14, (unsigned long)(o.n - 0x14));
            buf_add(&o, conv->p + 0x14 + g, conv->n - (0x14 + g));
        } else wr32(&o, 0x14, 0);
        wr32(&o, 0x04, (unsigned long)(o.n - 4));
        res->n = 0;
        buf_add(res, o.p, o.n);
        buf_free(&o);
    }
    {
        int np = 0, no = 0, q;
        for (q = 0; q < g_nsmp; q++) if (g_smp[q].band != -1000) { if (g_smp[q].pcm) np++; else no++; }
        if (g_md_pwmonly) sprintf(msg, g_en ? "32X PWM sampled: %ld sounds, %ld KB" : "PWM 32X grabado: %ld sonidos, %ld KB", g_npev / 2, (g_bank.n + 1023) / 1024);
        else sprintf(msg, g_en ? "sampled FM: %d instruments by PCM, %d by OPL3, %ld KB" : "FM grabado: %d instrumentos por PCM, %d por OPL3, %ld KB", np, no, (g_bank.n + 1023) / 1024);
        if (g_md_maxch > 0 && !g_md_pwmonly) {
            char m2[96];
            sprintf(m2, g_en ? "  (-c%d: %ld notes to OPL3, %ld taken over)" : "  (-c%d: %ld notas pasadas al OPL3, %ld cedidas)", g_md_maxch, g_n_fall, g_n_steal);
            strcat(msg, m2);
        }
    }
    buf_free(&g_bank); buf_free(&g_evdata); buf_free(&g_dbank);
    free(g_smp); free(g_ev); free(g_dev); free(g_dsm); free(g_mode); free(g_pev); g_pev = NULL;
    return 1;
}

int mdpcm_tiene_pwm(const Buf *orig)
{
    long doff = (rd32(orig, 8) >= 0x150 && rd32(orig, 0x34)) ? 0x34 + (long)rd32(orig, 0x34) : 0x40, i, n = 0;
    for (i = doff; i < orig->n && n < 200; ) {
        int c = orig->p[i];
        if (c == 0x66) break;
        if (c == 0x67) { i += 7 + (long)(rd32(orig, i + 3) & 0x7FFFFFFFUL); continue; }
        if (c == 0xB2) n++;
        i += lz_largo(orig->p, orig->n, i);
    }
    return n >= 200;
}

static int rec_cmp(const void *a, const void *b)
{
    const NRec *x = *(NRec * const *)a, *y = *(NRec * const *)b;
    if (x->prio != y->prio) return x->prio < y->prio ? 1 : -1;
    if ((x->tend - x->t) != (y->tend - y->t)) return (x->tend - x->t) < (y->tend - y->t) ? -1 : 1;
    return x->t < y->t ? -1 : (x->t > y->t);
}
typedef struct { long t; int d; } SwEv;
static int sw_cmp(const void *a, const void *b)
{
    const SwEv *x = (const SwEv *)a, *y = (const SwEv *)b;
    if (x->t != y->t) return x->t < y->t ? -1 : 1;
    return x->d - y->d;
}

int mdpcm_convertir(const Buf *orig, const Buf *conv, Buf *res, char *msg)
{
    Buf tmp;
    int ok, N = g_md_maxch, k;
    char m1[200];
    if (N <= 0 || g_md_pwmonly) return mdpcm_convertir_in(orig, conv, res, msg);
    g_md_maxch = 0; g_need_score = 1; g_pass = 1;
    g_nrec_a = NULL; g_nrec = g_nrec_cap = 0;
    for (k = 0; k < 6; k++) g_recopen[k] = -1;
    buf_init(&tmp);
    ok = mdpcm_convertir_in(orig, conv, &tmp, m1);
    buf_free(&tmp);
    g_md_maxch = N; g_pass = 0;
    if (!ok || g_nrec == 0) { g_need_score = 0; free(g_nrec_a); g_nrec_a = NULL; g_nrec = 0; return mdpcm_convertir_in(orig, conv, res, msg); }
    {
        long i, a, na = 0, nfall = 0;
        NRec **ord = (NRec **)xmalloc(sizeof(NRec *) * (size_t)g_nrec);
        NRec **acc = (NRec **)xmalloc(sizeof(NRec *) * (size_t)g_nrec);
        SwEv *sw = (SwEv *)xmalloc(sizeof(SwEv) * (size_t)(2 * g_nrec + 2));
        for (i = 0; i < g_nrec; i++) { if (g_nrec_a[i].tend < 0) g_nrec_a[i].tend = 0x7FFFFFFFL; ord[i] = &g_nrec_a[i]; }
        qsort(ord, (size_t)g_nrec, sizeof(NRec *), rec_cmp);
        for (i = 0; i < g_nrec; i++) {
            NRec *c = ord[i];
            long ns = 0, cur = 0, mx = 0, q;
            for (a = 0; a < na; a++)
                if (acc[a]->t < c->tend && acc[a]->tend > c->t) {
                    sw[ns].t = acc[a]->t < c->t ? c->t : acc[a]->t; sw[ns].d = 1; ns++;
                    sw[ns].t = acc[a]->tend > c->tend ? c->tend : acc[a]->tend; sw[ns].d = -1; ns++;
                }
            qsort(sw, (size_t)ns, sizeof(SwEv), sw_cmp);
            for (q = 0; q < ns; q++) { cur += sw[q].d; if (cur > mx) mx = cur; }
            if (mx < N) { acc[na++] = c; c->pcm = 1; }
            else { c->pcm = 0; nfall++; }
        }
        free(ord); free(acc); free(sw);
        g_n_fall = nfall;
    }
    g_pass = 2; g_rpos = 0;
    ok = mdpcm_convertir_in(orig, conv, res, msg);
    g_pass = 0; g_need_score = 0;
    g_n_fall = 0;
    free(g_nrec_a); g_nrec_a = NULL; g_nrec = 0;
    return ok;
}
