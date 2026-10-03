#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "vgmlib.h"
#include "mdconv.h"
#include "ngconv.h"

extern int g_en;
int ngopl_convertir(const Buf *d, Buf *res, char *msg);

#define RF_CLOCK   12500001UL
#define RF_RATE    (12500000.0 / 384.0)
#define RING       0x2000L
#define RING_DATA  (RING - 1)
#define CHUNK      512L
#define NCH        7
#define DECIM      2

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

static const short a_steps[49] = {
    16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
    494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552 };
static const signed char a_inc[8] = { -1, -1, -1, -1, 2, 5, 7, 9 };

static long dec_a(const unsigned char *rom, long n, int *out)
{
    long i, k = 0;
    int acc = 0, step = 0;
    for (i = 0; i < n; i++) {
        int sh;
        for (sh = 4; sh >= 0; sh -= 4) {
            int d = (rom[i] >> sh) & 15;
            int delta = (2 * (d & 7) + 1) * a_steps[step] / 8;
            if (d & 8) delta = -delta;
            acc = ((acc + delta + 2048) & 0xFFF) - 2048;
            out[k++] = acc;
            step += a_inc[d & 7];
            if (step < 0) step = 0;
            if (step > 48) step = 48;
        }
    }
    return k;
}

static const int b_steps[8] = { 57, 57, 57, 57, 77, 102, 128, 153 };

static long dec_b(const unsigned char *rom, long n, int *out)
{
    long i, k = 0, acc = 0, stp = 127;
    for (i = 0; i < n; i++) {
        int sh;
        for (sh = 4; sh >= 0; sh -= 4) {
            int d = (rom[i] >> sh) & 15;
            long delta = ((2 * (d & 7) + 1) * stp) >> 3;
            if (d & 8) acc -= delta; else acc += delta;
            if (acc > 32767) acc = 32767;
            if (acc < -32768) acc = -32768;
            stp = (stp * b_steps[d & 7]) >> 6;
            if (stp < 127) stp = 127;
            if (stp > 24576) stp = 24576;
            out[k++] = (int)acc;
        }
    }
    return k;
}

typedef struct { int type; long start, end; long off, len; double peak; int dec; double hot; } Smp;
static Smp *g_smp; static int g_nsmp, g_capsmp;
static Buf g_bank;

static unsigned char sm_byte(int v)
{
    if (v >= 0) return (unsigned char)(v > 127 ? 127 : v);
    v = -v;
    if (v > 126) v = 126;
    return (unsigned char)(0x80 | v);
}

int g_ng_full = 1;

#define NTAP 33
static double g_h[NTAP];
static int g_hdec = 0;

static void make_fir(int dec)
{
    int i;
    double fc = 0.45 / dec, sum = 0;
    if (g_hdec == dec) return;
    g_hdec = dec;
    for (i = 0; i < NTAP; i++) {
        int m = i - NTAP / 2;
        double v = m ? sin(2.0 * 3.14159265358979 * fc * m) / (3.14159265358979 * m) : 2.0 * fc;
        v *= 0.54 - 0.46 * cos(2.0 * 3.14159265358979 * i / (NTAP - 1));
        g_h[i] = v; sum += v;
    }
    for (i = 0; i < NTAP; i++) g_h[i] /= sum;
}

static double lp_at(const int *x, long n, long k)
{
    int i;
    double v = 0;
    for (i = 0; i < NTAP; i++) {
        long j = k + i - NTAP / 2;
        if (j >= 0 && j < n) v += g_h[i] * x[j];
    }
    return v;
}

static double hf_loss(const int *x, long n, int dec)
{
    long k;
    double mean = 0, e = 0, el = 0;
    if (dec <= 1 || n < 64) return 0;
    for (k = 0; k < n; k++) mean += x[k];
    mean /= n;
    make_fir(dec);
    for (k = 0; k < n; k++) {
        double a = x[k] - mean, b = lp_at(x, n, k) - mean;
        e += a * a; el += b * b;
    }
    if (e <= 0) return 0;
    el /= e;
    return el >= 1 ? 0 : 1 - el;
}

#define HF_EMPH 0.5
#define HF_MAX 0.15

typedef struct { long start, end; double minrate; } BUse;
static BUse *g_buse; static int g_nbuse;

static double b_minrate(long start, long end)
{
    int i;
    for (i = 0; i < g_nbuse; i++)
        if (g_buse[i].start == start && g_buse[i].end == end) return g_buse[i].minrate;
    return 22050.0;
}

static int b_decim(const int *raw, long n, long start, long end)
{
    double mr = b_minrate(start, end);
    int f = (int)(mr / 11025.0), f2 = (int)(mr / 18500.0 + 0.5);
    if (f < 2) f = 2;
    if (f > 6) f = 6;
    if (f2 < 1) f2 = 1;
    if (f2 > 6) f2 = 6;
    if (g_ng_full || hf_loss(raw, n, f) > HF_MAX) return f2;
    return f;
}

static int get_smp(int type, long start, long end, const Buf *rom)
{
    int i, dec;
    long nbytes, n, k, m;
    int *raw;
    double peak = 0, *lp;
    Smp *s;
    long req_end = end;
    for (i = 0; i < g_nsmp; i++)
        if (g_smp[i].type == type && g_smp[i].start == start && g_smp[i].end == req_end) return i;
    if (type == 0) {
        long e = (start & ~0xFFFFFL) | (end & 0xFFFFFL);
        if (e < start) e += 0x100000L;
        end = e;
    }
    if (start >= rom->n) return -1;
    if (end >= rom->n) end = rom->n - 1;
    if (end < start) return -1;
    nbytes = end - start + 1;
    raw = (int *)xmalloc(sizeof(int) * (size_t)(nbytes * 2 + 2));
    n = (type == 0) ? dec_a(rom->p + start, nbytes, raw) : dec_b(rom->p + start, nbytes, raw);
    if (type == 0)
        dec = (g_ng_full || hf_loss(raw, n, DECIM) > HF_MAX) ? 1 : DECIM;
    else
        dec = b_decim(raw, n, start, end);
    m = n / dec;
    lp = (double *)xmalloc(sizeof(double) * (size_t)(m + 1));
    if (dec > 1) make_fir(dec);
    for (k = 0; k < m; k++) lp[k] = (dec > 1) ? lp_at(raw, n, k * dec) : raw[k];
    if (dec == 1 && m > 2) {
        double prev = lp[0], cur;
        for (k = 0; k < m; k++) {
            double nx = (k + 1 < m) ? lp[k + 1] : lp[k];
            cur = lp[k];
            lp[k] = cur + HF_EMPH * (cur - 0.5 * (prev + nx));
            prev = cur;
        }
    }
    for (k = 0; k < m; k++) if (fabs(lp[k]) > peak) peak = fabs(lp[k]);
    if (g_nsmp >= g_capsmp) {
        g_capsmp = g_capsmp ? g_capsmp * 2 : 64;
        g_smp = (Smp *)xrealloc(g_smp, sizeof(Smp) * (size_t)g_capsmp);
    }
    s = &g_smp[g_nsmp];
    s->type = type; s->start = start; s->end = req_end;
    s->off = g_bank.n; s->len = m; s->peak = peak; s->dec = dec; s->hot = 0;
    for (k = 0; k < m; k++) {
        int q = peak > 0 ? (int)floor(lp[k] * 127.0 / peak + 0.5) : 0;
        buf_byte(&g_bank, sm_byte(q));
    }
    for (k = 0; k < 64; k++) buf_byte(&g_bank, 0);
    free(raw); free(lp);
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

static void e3(long t, int a, int b, int c)
{
    unsigned char x[3];
    x[0] = (unsigned char)a; x[1] = (unsigned char)b; x[2] = (unsigned char)c;
    emit(t, x, 3);
}

static void rf_reg(long t, int r, int v) { e3(t, 0xB1, r, v); }

typedef struct { long evoff, ev; int smp; long si; } CRef;
static CRef *g_cref; static long g_ncref, g_capcref;

static void rf_copy(long t, int wbank, int smp, long si, long dst, long n)
{
    unsigned char x[12];
    long src = si;
    rf_reg(t, 7, 0x80 | wbank);
    if (g_ncref >= g_capcref) {
        g_capcref = g_capcref ? g_capcref * 2 : 4096;
        g_cref = (CRef *)xrealloc(g_cref, sizeof(CRef) * (size_t)g_capcref);
    }
    g_cref[g_ncref].evoff = g_evdata.n; g_cref[g_ncref].ev = g_nev; g_cref[g_ncref].smp = smp; g_cref[g_ncref].si = si;
    g_ncref++;
    g_smp[smp].hot += n;
    x[0] = 0x68; x[1] = 0x66; x[2] = 0x02;
    x[3] = (unsigned char)src; x[4] = (unsigned char)(src >> 8); x[5] = (unsigned char)(src >> 16);
    x[6] = (unsigned char)dst; x[7] = (unsigned char)(dst >> 8); x[8] = (unsigned char)(dst >> 16);
    x[9] = (unsigned char)n; x[10] = (unsigned char)(n >> 8); x[11] = (unsigned char)(n >> 16);
    emit(t, x, 12);
}

typedef struct { long t; long ev; int ch; int kind; } PEv;
static PEv *g_pev; static long g_npev, g_cappev;

static void pitch_note(long t, int port, int reg, int val)
{
    PEv *p;
    if (g_npev >= g_cappev) {
        g_cappev = g_cappev ? g_cappev * 2 : 4096;
        g_pev = (PEv *)xrealloc(g_pev, sizeof(PEv) * (size_t)g_cappev);
    }
    p = &g_pev[g_npev++];
    p->t = t; p->ev = g_nev;
    if (reg == 0x28) { p->kind = 2; p->ch = (val & 3) + ((val & 4) ? 3 : 0); }
    else { p->kind = (reg >= 0xA4) ? 0 : 1; p->ch = (reg & 3) + port * 3; }
}

#define THIN_GAP (44100L * 8 / 1000)

static void thin_pitch(void)
{
    int ch;
    for (ch = 0; ch < 6; ch++) {
        long i, kept_t = -0x7FFFFFFFL, a4 = -1, pend_a4 = -1;
        long cur_a0 = -1, cur_a4 = -1;
        for (i = 0; i < g_npev; i++) {
            PEv *p = &g_pev[i];
            long j, nxt = -1, nxt_has_a4 = 0, key_between = 0;
            if (p->ch != ch) continue;
            if (p->kind == 2) { kept_t = -0x7FFFFFFFL; cur_a4 = -1; continue; }
            if (p->kind == 0) { cur_a4 = i; continue; }
            cur_a0 = i;
            for (j = i + 1; j < g_npev; j++) {
                PEv *q = &g_pev[j];
                if (q->ch != ch) continue;
                if (q->kind == 2) { key_between = 1; break; }
                if (q->kind == 0) { nxt_has_a4 = 1; continue; }
                nxt = j; break;
            }
            if (!key_between && nxt >= 0 && g_pev[nxt].t - p->t < THIN_GAP &&
                p->t - kept_t < THIN_GAP && (cur_a4 < 0 || nxt_has_a4)) {
                g_ev[p->ev].len = 0;
                if (cur_a4 >= 0) g_ev[g_pev[cur_a4].ev].len = 0;
            } else kept_t = p->t;
            cur_a4 = -1;
        }
        (void)a4; (void)pend_a4; (void)cur_a0;
    }
}

#define BLK 2048L
typedef struct { int smp; long b; double hot; } Blk;
static Blk *g_blk;
static int cmp_blk(const void *a, const void *b)
{
    const Blk *x = (const Blk *)a, *y = (const Blk *)b;
    if (x->hot != y->hot) return x->hot > y->hot ? -1 : 1;
    if (x->smp != y->smp) return x->smp - y->smp;
    return (x->b > y->b) - (x->b < y->b);
}

static void reorder_bank(void)
{
    Buf nb;
    long i, nblk = 0, *first, *noff;
    int k;
    if (g_nsmp == 0) return;
    first = (long *)xmalloc(sizeof(long) * (size_t)(g_nsmp + 1));
    for (k = 0; k < g_nsmp; k++) {
        first[k] = nblk;
        nblk += (g_smp[k].len + 64 + BLK - 1) / BLK;
    }
    first[g_nsmp] = nblk;
    g_blk = (Blk *)xmalloc(sizeof(Blk) * (size_t)nblk);
    noff = (long *)xmalloc(sizeof(long) * (size_t)nblk);
    for (k = 0; k < g_nsmp; k++) {
        long b;
        for (b = 0; b < first[k + 1] - first[k]; b++) {
            g_blk[first[k] + b].smp = k; g_blk[first[k] + b].b = b; g_blk[first[k] + b].hot = 0;
        }
    }
    for (i = 0; i < g_ncref; i++) {
        const unsigned char *x = g_evdata.p + g_cref[i].evoff;
        long n = x[9] | ((long)x[10] << 8) | ((long)x[11] << 16);
        long si = g_cref[i].si;
        while (n > 0) {
            long b = si / BLK, m = (b + 1) * BLK - si;
            if (m > n) m = n;
            g_blk[first[g_cref[i].smp] + b].hot += m;
            si += m; n -= m;
        }
    }
    qsort(g_blk, (size_t)nblk, sizeof(Blk), cmp_blk);
    buf_init(&nb);
    for (i = 0; i < nblk; i++) {
        const Smp *s = &g_smp[g_blk[i].smp];
        long st = g_blk[i].b * BLK, m = s->len + 64 - st;
        if (m > BLK) m = BLK;
        noff[first[g_blk[i].smp] + g_blk[i].b] = nb.n;
        buf_add(&nb, g_bank.p + s->off + st, m);
    }
    for (i = 0; i < g_ncref; i++) {
        unsigned char x[12];
        long n, si = g_cref[i].si, dst, ev = g_cref[i].ev, off0 = g_evdata.n;
        memcpy(x, g_evdata.p + g_cref[i].evoff, 12);
        n = x[9] | ((long)x[10] << 8) | ((long)x[11] << 16);
        dst = x[6] | ((long)x[7] << 8) | ((long)x[8] << 16);
        while (n > 0) {
            long b = si / BLK, m = (b + 1) * BLK - si, src;
            if (m > n) m = n;
            src = noff[first[g_cref[i].smp] + b] + (si - b * BLK);
            x[3] = (unsigned char)src; x[4] = (unsigned char)(src >> 8); x[5] = (unsigned char)(src >> 16);
            x[6] = (unsigned char)dst; x[7] = (unsigned char)(dst >> 8); x[8] = (unsigned char)(dst >> 16);
            x[9] = (unsigned char)m; x[10] = (unsigned char)(m >> 8); x[11] = (unsigned char)(m >> 16);
            buf_add(&g_evdata, x, 12);
            si += m; dst += m; n -= m;
        }
        g_ev[ev].off = off0; g_ev[ev].len = (int)(g_evdata.n - off0);
    }
    buf_free(&g_bank);
    g_bank = nb;
    free(g_blk); free(noff); free(first);
}

static int cmp_ev(const void *a, const void *b)
{
    const Ev *x = (const Ev *)a, *y = (const Ev *)b;
    if (x->t != y->t) return x->t < y->t ? -1 : 1;
    return (x->seq > y->seq) - (x->seq < y->seq);
}

typedef struct {
    int on;
    int smp;
    int loop;
    double rate;
    double pos0;
    long t0;
    long written;
    long tend;
    int env, pan;
    unsigned fd;
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

static void ch_write(int k, long t, long b, long n)
{
    Ch *c = &g_ch[k];
    const Smp *s = &g_smp[c->smp];
    while (n > 0) {
        long si, rp, m;
        if (c->loop) si = b % s->len;
        else {
            if (b >= s->len + 64) return;
            si = b;
        }
        rp = b % RING_DATA;
        m = n;
        if (m > RING_DATA - rp) m = RING_DATA - rp;
        if (c->loop) { if (m > s->len - si) m = s->len - si; }
        else if (m > s->len + 64 - si) m = s->len + 64 - si;
        rf_copy(t, k * 2, c->smp, si, rp, m);
        b += m; n -= m;
    }
}

static void flush_until(long t)
{
    int k;
    for (k = 0; k < NCH; k++) {
        Ch *c = &g_ch[k];
        const Smp *s;
        long total;
        if (!c->on) continue;
        s = &g_smp[c->smp];
        total = c->loop ? 0x7FFFFFFFL : s->len + 64;
        for (;;) {
            long b = c->written, tw;
            double need;
            if (b >= total) break;
            need = (double)(b - ch_lead(c));
            tw = c->t0 + (long)ceil((need - c->pos0) * 44100.0 / c->rate);
            tw = tw / 220 * 220;
            if (tw < c->t0) tw = c->t0;
            if (tw > t || (!c->loop && tw >= c->tend)) break;
            ch_write(k, tw, b, CHUNK);
            c->written += CHUNK;
        }
        if (!c->loop && c->tend <= t) {
            g_mask |= 1u << k;
            rf_reg(c->tend, 8, (int)g_mask);
            c->on = 0;
        }
    }
}

static void ch_regs(long t, int k)
{
    Ch *c = &g_ch[k];
    rf_reg(t, 7, 0xC0 | k);
    rf_reg(t, 0, c->env);
    rf_reg(t, 1, c->pan);
    rf_reg(t, 2, c->fd & 0xFF);
    rf_reg(t, 3, c->fd >> 8);
}

static void ch_start(int k, long t, int smp, double rate, int loop, int env, int pan)
{
    Ch *c = &g_ch[k];
    long first;
    g_mask |= 1u << k;
    rf_reg(t, 8, (int)g_mask);
    c->on = 1; c->smp = smp; c->loop = loop; c->rate = rate;
    c->pos0 = 0; c->t0 = t; c->written = 0;
    c->env = env; c->pan = pan; c->fd = rate_fd(rate);
    c->tend = loop ? 0x7FFFFFFFL : t + (long)ceil((double)g_smp[smp].len * 44100.0 / rate) + 1;
    first = ch_lead(c) + CHUNK;
    first = (first + CHUNK - 1) / CHUNK * CHUNK;
    if (!loop && first > g_smp[smp].len + 64) first = g_smp[smp].len + 64;
    ch_write(k, t, 0, first);
    c->written = first;
    ch_regs(t, k);
    rf_reg(t, 4, (int)((k * RING) & 0xFF));
    rf_reg(t, 5, (int)(((k * RING) >> 8) & 0xFF));
    rf_reg(t, 6, (int)((k * RING) >> 8));
    g_mask &= ~(1u << k);
    rf_reg(t, 8, (int)g_mask);
}

static void ch_stop(int k, long t)
{
    if (!g_ch[k].on) return;
    g_ch[k].on = 0;
    g_mask |= 1u << k;
    rf_reg(t, 8, (int)g_mask);
}

static int pan_rf(int lr)
{
    int p = 0;
    if (lr & 0x80) p |= 0x0F;
    if (lr & 0x40) p |= 0xF0;
    return p;
}

static int env_clip(double v)
{
    int e = (int)floor(v + 0.5);
    return e < 0 ? 0 : (e > 255 ? 255 : e);
}

static double gain_a(int lvl, int tl)
{
    int att = ((lvl & 31) ^ 31) + ((tl & 63) ^ 63);
    return pow(2.0, -att / 8.0);
}

#define KA (255.0 / 2048.0)
#define KB (KA / 15.0 / 256.0)

int ng_convertir(const Buf *d, Buf *res, char *msg)
{
    unsigned long clk;
    long doff, i, n = d->n, t = 0, loop_abs, loop_t = -1, g;
    Buf romA, romB, syn;
    unsigned char ra[0x30], rb[0x20];
    int k, r;
    long loop_syn = -1;
    double arate;
    msg[0] = 0;
    if (n < 0x100 || memcmp(d->p, "Vgm ", 4) != 0 || rd32(d, 8) < 0x151) return 0;
    clk = rd32(d, 0x4C) & 0x3FFFFFFFUL;
    if (!clk) return 0;
    doff = rd32(d, 0x34) ? 0x34 + (long)rd32(d, 0x34) : 0x40;
    loop_abs = rd32(d, 0x1C) ? 0x1C + (long)rd32(d, 0x1C) : -1;
    buf_init(&romA); buf_init(&romB); buf_init(&syn); buf_init(&g_bank); buf_init(&g_evdata);
    g_smp = NULL; g_nsmp = g_capsmp = 0; g_cref = NULL; g_ncref = g_capcref = 0; g_pev = NULL; g_npev = g_cappev = 0; g_ev = NULL; g_nev = g_capev = g_seq = 0;
    memset(g_ch, 0, sizeof(g_ch));
    memset(ra, 0, sizeof(ra)); memset(rb, 0, sizeof(rb));
    g_mask = 0xFF;
    arate = (double)clk / 432.0;
    for (i = doff; i < n; ) {
        int c = d->p[i];
        if (c == 0x66) break;
        if (c == 0x67) {
            long sz = (long)(rd32(d, i + 3) & 0x7FFFFFFFUL);
            int ty = d->p[i + 2];
            if ((ty == 0x82 || ty == 0x83) && sz > 8) {
                Buf *rom = (ty == 0x82) ? &romA : &romB;
                long st = (long)rd32(d, i + 11), len = sz - 8;
                if (i + 15 + len > n) len = n - (i + 15);
                if (st + len > rom->n) {
                    long o = rom->n;
                    buf_reserve(rom, st + len);
                    memset(rom->p + o, 0, (size_t)(st + len - o));
                    rom->n = st + len;
                }
                memcpy(rom->p + st, d->p + i + 15, (size_t)len);
            }
            i += 7 + sz;
        } else i += lz_largo(d->p, n, i);
    }
    g_buse = NULL; g_nbuse = 0;
    {
        unsigned char b2[0x20];
        memset(b2, 0, sizeof(b2));
        for (i = doff; i < n; ) {
            int c = d->p[i];
            if (c == 0x66) break;
            if (c == 0x67) { i += 7 + (long)(rd32(d, i + 3) & 0x7FFFFFFFUL); continue; }
            if (c == 0x58 && i + 2 < n && d->p[i + 1] >= 0x10 && d->p[i + 1] < 0x20) {
                int reg = d->p[i + 1], val = d->p[i + 2];
                b2[reg - 0x10] = (unsigned char)val;
                if ((reg == 0x10 && (val & 0x80)) || reg == 0x19 || reg == 0x1A) {
                    long st = (long)(b2[2] | (b2[3] << 8)) << 8;
                    long en = ((long)(b2[4] | (b2[5] << 8)) << 8) | 0xFF;
                    double rate = (double)clk / 144.0 * (b2[9] | (b2[10] << 8)) / 65536.0;
                    int j;
                    if (rate > 1000) {
                        for (j = 0; j < g_nbuse; j++)
                            if (g_buse[j].start == st && g_buse[j].end == en) break;
                        if (j == g_nbuse) {
                            g_buse = (BUse *)xrealloc(g_buse, sizeof(BUse) * (size_t)(g_nbuse + 1));
                            g_buse[j].start = st; g_buse[j].end = en; g_buse[j].minrate = rate;
                            g_nbuse++;
                        } else if (reg == 0x10 && rate < g_buse[j].minrate) g_buse[j].minrate = rate;
                    }
                }
            }
            i += lz_largo(d->p, n, i);
        }
    }
    rf_reg(0, 7, 0x80);
    rf_reg(0, 8, 0xFF);
    for (k = 0; k < NCH; k++) {
        unsigned char x[4];
        rf_reg(0, 7, 0x80 | (k * 2 + 1));
        x[0] = 0xC2; x[1] = 0xFF; x[2] = 0x0F; x[3] = 0xFF;
        emit(0, x, 4);
    }
    for (i = doff; i < n; ) {
        int c = d->p[i];
        if (i == loop_abs) loop_t = t;
        if (c == 0x66) break;
        if (c == 0x61) { t += d->p[i + 1] | (d->p[i + 2] << 8); i += 3; continue; }
        if (c == 0x62) { t += 735; i++; continue; }
        if (c == 0x63) { t += 882; i++; continue; }
        if (c >= 0x70 && c <= 0x7F) { t += (c & 15) + 1; i++; continue; }
        if (c == 0x67) { i += 7 + (long)(rd32(d, i + 3) & 0x7FFFFFFFUL); continue; }
        if ((c == 0x58 || c == 0x59) && i + 2 < n) {
            int port = c - 0x58, reg = d->p[i + 1], val = d->p[i + 2];
            flush_until(t);
            if (port == 0 && reg < 0x10) {
                if (reg >= 8 && reg <= 10 && !(val & 0x10))
                    val = (val & 15) > 2 ? (val & 15) - 2 : 0;
                e3(t, 0xA0, reg, val);
            } else if (port == 0 && reg < 0x20) {
                rb[reg - 0x10] = (unsigned char)val;
                if (reg == 0x10) {
                    if (val & 0x01) ch_stop(6, t);
                    else if (val & 0x80) {
                        long st = (long)(rb[2] | (rb[3] << 8)) << 8;
                        long en = ((long)(rb[4] | (rb[5] << 8)) << 8) | 0xFF;
                        int sm = get_smp(1, st, en, &romB);
                        unsigned dn = rb[9] | (rb[10] << 8);
                        double rate = sm >= 0 ? (double)clk / 144.0 * dn / 65536.0 / g_smp[sm].dec : 0;
                        if (sm >= 0 && rate > 0 && g_smp[sm].len > 0)
                            ch_start(6, t, sm, rate, (val & 0x10) != 0,
                                     env_clip(KB * rb[0x0B] * g_smp[sm].peak), pan_rf(rb[1]));
                    } else ch_stop(6, t);
                } else if (reg == 0x11 && g_ch[6].on) {
                    g_ch[6].pan = pan_rf(val);
                    rf_reg(t, 7, 0xC6); rf_reg(t, 1, g_ch[6].pan);
                } else if ((reg == 0x19 || reg == 0x1A) && g_ch[6].on) {
                    unsigned dn = rb[9] | (rb[10] << 8);
                    Ch *cc = &g_ch[6];
                    double rate = (double)clk / 144.0 * dn / 65536.0 / g_smp[cc->smp].dec;
                    if (rate > 0) {
                        cc->pos0 = ch_pos(cc, t); cc->t0 = t; cc->rate = rate;
                        cc->fd = rate_fd(rate);
                        if (!cc->loop)
                            cc->tend = t + (long)ceil(((double)g_smp[cc->smp].len - cc->pos0) * 44100.0 / rate) + 1;
                        rf_reg(t, 7, 0xC6); rf_reg(t, 2, cc->fd & 0xFF); rf_reg(t, 3, cc->fd >> 8);
                    }
                } else if (reg == 0x1B && g_ch[6].on) {
                    g_ch[6].env = env_clip(KB * val * g_smp[g_ch[6].smp].peak);
                    rf_reg(t, 7, 0xC6); rf_reg(t, 0, g_ch[6].env);
                }
            } else if (port == 1 && reg < 0x30) {
                ra[reg] = (unsigned char)val;
                if (reg == 0x00) {
                    for (k = 0; k < 6; k++) {
                        if (!(val & (1 << k))) continue;
                        if (val & 0x80) ch_stop(k, t);
                        else {
                            long st = (long)(ra[0x10 + k] | (ra[0x18 + k] << 8)) << 8;
                            long en = ((long)(ra[0x20 + k] | (ra[0x28 + k] << 8)) << 8) | 0xFF;
                            int sm = get_smp(0, st, en, &romA);
                            if (sm >= 0 && g_smp[sm].len > 0)
                                ch_start(k, t, sm, arate / g_smp[sm].dec, 0,
                                         env_clip(KA * gain_a(ra[8 + k], ra[1]) * g_smp[sm].peak),
                                         pan_rf(ra[8 + k]));
                        }
                    }
                } else if (reg == 0x01) {
                    for (k = 0; k < 6; k++) if (g_ch[k].on) {
                        g_ch[k].env = env_clip(KA * gain_a(ra[8 + k], val) * g_smp[g_ch[k].smp].peak);
                        rf_reg(t, 7, 0xC0 | k); rf_reg(t, 0, g_ch[k].env);
                    }
                } else if (reg >= 0x08 && reg <= 0x0D) {
                    k = reg - 0x08;
                    if (g_ch[k].on) {
                        g_ch[k].env = env_clip(KA * gain_a(val, ra[1]) * g_smp[g_ch[k].smp].peak);
                        g_ch[k].pan = pan_rf(val);
                        rf_reg(t, 7, 0xC0 | k); rf_reg(t, 0, g_ch[k].env); rf_reg(t, 1, g_ch[k].pan);
                    }
                }
            } else {
                if ((reg >= 0xA0 && reg <= 0xA2) || (reg >= 0xA4 && reg <= 0xA6) || reg == 0x28)
                    pitch_note(t, port, reg, val);
                e3(t, port ? 0x53 : 0x52, reg, val);
            }
            i += 3;
            continue;
        }
        i += lz_largo(d->p, n, i);
    }
    {
        long end_t = t > (long)rd32(d, 0x18) ? t : (long)rd32(d, 0x18);
        flush_until(end_t);
        t = end_t;
    }
    thin_pitch();
    reorder_bank();
    qsort(g_ev, (size_t)g_nev, sizeof(Ev), cmp_ev);
    buf_reserve(&syn, 0x100);
    memset(syn.p, 0, 0x100); syn.n = 0x100;
    memcpy(syn.p, "Vgm ", 4);
    wr32(&syn, 0x08, 0x171);
    wr32(&syn, 0x18, (unsigned long)t);
    wr32(&syn, 0x2C, clk);
    wr32(&syn, 0x34, 0x100 - 0x34);
    wr32(&syn, 0x6C, RF_CLOCK);
    wr32(&syn, 0x74, clk / 4);
    syn.p[0x78] = 0x10;
    buf_byte(&syn, 0x67); buf_byte(&syn, 0x66); buf_byte(&syn, 0x02);
    buf_u32(&syn, (unsigned long)g_bank.n);
    buf_add(&syn, g_bank.p, g_bank.n);
    {
        long cur = 0;
        for (i = 0; i < g_nev; i++) {
            long w;
            if (loop_t >= 0 && loop_syn < 0 && g_ev[i].t >= loop_t) {
                w = loop_t - cur;
                while (w > 0) { long q = w > 65535 ? 65535 : w; buf_byte(&syn, 0x61); buf_u16(&syn, (unsigned)q); w -= q; }
                cur = loop_t;
                loop_syn = syn.n;
            }
            w = g_ev[i].t - cur;
            while (w > 0) { long q = w > 65535 ? 65535 : w; buf_byte(&syn, 0x61); buf_u16(&syn, (unsigned)q); w -= q; }
            if (g_ev[i].t > cur) cur = g_ev[i].t;
            buf_add(&syn, g_evdata.p + g_ev[i].off, g_ev[i].len);
        }
        if (t > cur) { long w = t - cur; while (w > 0) { long q = w > 65535 ? 65535 : w; buf_byte(&syn, 0x61); buf_u16(&syn, (unsigned)q); w -= q; } }
    }
    buf_byte(&syn, 0x66);
    if (loop_syn >= 0) {
        wr32(&syn, 0x1C, (unsigned long)(loop_syn - 0x1C));
        wr32(&syn, 0x20, (unsigned long)(t - loop_t));
    }
    g = (long)rd32(d, 0x14);
    if (g && 0x18 + g <= n && memcmp(d->p + 0x14 + g, "Gd3 ", 4) == 0) {
        wr32(&syn, 0x14, (unsigned long)(syn.n - 0x14));
        buf_add(&syn, d->p + 0x14 + g, n - (0x14 + g));
    }
    wr32(&syn, 0x04, (unsigned long)(syn.n - 4));
    r = ngopl_convertir(&syn, res, msg);
    if (r == 0) {
        wr32(&syn, 0x2C, 0);
        res->n = 0;
        buf_add(res, syn.p, syn.n);
        r = 1;
    }
    {
        char tmp[200];
        sprintf(tmp, g_en ? "Neo Geo, PCM %ld KB" : "Neo Geo, PCM %ld KB", (g_bank.n + 1023) / 1024);
        if (msg[0]) { strcat(tmp, ", "); strncat(tmp, msg, 150); }
        strcpy(msg, tmp);
    }
    buf_free(&romA); buf_free(&romB); buf_free(&syn); buf_free(&g_bank); buf_free(&g_evdata);
    free(g_smp); free(g_ev); free(g_buse); free(g_cref); free(g_pev);
    return r;
}
