#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "vgmlib.h"
#include "mdconv.h"
extern int g_en;

#define NONE (-1)
static const int SLOTN[5] = { 0, 0, 2, 1, 3 };
static const int OPOFF[9] = { 0, 1, 2, 8, 9, 10, 16, 17, 18 };
static const int MUL_MAP[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 10, 12, 12, 15, 15 };
static const double OPL_MULT[16] = { 0.5, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 10, 12, 12, 15, 15 };
#define EG_OFFSET 5.5
static const int PAIRS[6][2] = { {0, 3}, {1, 4}, {2, 5}, {9, 12}, {10, 13}, {11, 14} };
static const int SPARE[6] = { 6, 7, 8, 15, 16, 17 };
#define YMCLK_DEFAULT 7670453UL
#define OPLCLK 14318180UL
static const int LFO_SPS[8] = { 108, 77, 71, 67, 62, 44, 8, 5 };
static const double PMS_CENTS[8] = { 0, 3.4, 6.7, 10, 14, 20, 40, 80 };
static const int AMS_SHIFT[4] = { 8, 3, 1, 0 };
#define LFO_UPDATE 441
#define DUAL_BOOST 4
#define DAC_GAP_MAX 64

#define CAR_BOOST 8
static int g_car_boost = CAR_BOOST;

static int g_dual_adj = 4;
#define DAC_VGMPLAY_VOL 0xF2
#define PSG_VGMPLAY_VOL 0xD8
static const long VDAC_RATES[2] = { 11025, 22050 };
#define VDAC_TOL 0.01
#define VDAC_GAIN (5.0 / 8.0)
#define VDAC_MAX 65000L
#define DAC_BANK_MAX 60000L

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

static double pyround(double x)
{
    double f = floor(x), r = x - f;
    if (r > 0.5) return f + 1.0;
    if (r < 0.5) return f;
    return (fmod(f, 2.0) == 0.0) ? f : f + 1.0;
}

static int ym_kc(int block, int fnum)
{
    int b10 = (fnum >> 10) & 1, b9 = (fnum >> 9) & 1, b8 = (fnum >> 8) & 1, b7 = (fnum >> 7) & 1;
    int n3 = (b10 & (b9 | b8 | b7)) | ((b10 ^ 1) & b9 & b8 & b7);
    return (block << 2) | (b10 << 1) | n3;
}

typedef struct { long t; unsigned char psg, port, reg, val; } YEv;
typedef struct { long t, pos; unsigned char v; } DacW;

typedef struct {
    unsigned long version, sn_clock, ym_clock, total;
    unsigned char sn_flags[4];
    int n_snflags;
    long loop_abs, loop_t, end_t;
    long gd3_off, gd3_len;
    unsigned long out_ym_clock;
    Buf bank;
    DacW *dacw; long ndac, capdac;
    struct RawE { long t, off, len; } *raw; long nraw, capraw;
    int has_ay;
    YEv *ev; long nev, capev;
} Info;

static void ev_add(Info *in, long t, int psg, int port, int reg, int val)
{
    YEv *e;
    if (in->nev >= in->capev) {
        in->capev = in->capev ? in->capev * 2 : 8192;
        in->ev = (YEv *)xrealloc(in->ev, sizeof(YEv) * (size_t)in->capev);
    }
    e = &in->ev[in->nev++];
    e->t = t; e->psg = (unsigned char)psg; e->port = (unsigned char)port;
    e->reg = (unsigned char)reg; e->val = (unsigned char)val;
}

static void dac_add(Info *in, long t, long pos, int v)
{
    if (in->ndac >= in->capdac) {
        in->capdac = in->capdac ? in->capdac * 2 : 8192;
        in->dacw = (DacW *)xrealloc(in->dacw, sizeof(DacW) * (size_t)in->capdac);
    }
    in->dacw[in->ndac].t = t; in->dacw[in->ndac].pos = pos; in->dacw[in->ndac].v = (unsigned char)v;
    in->ndac++;
}

static void raw_add(Info *in, long t, long off, long len)
{
    if (in->nraw >= in->capraw) {
        in->capraw = in->capraw ? in->capraw * 2 : 1024;
        in->raw = (struct RawE *)xrealloc(in->raw, sizeof(*in->raw) * (size_t)in->capraw);
    }
    in->raw[in->nraw].t = t; in->raw[in->nraw].off = off; in->raw[in->nraw].len = len;
    in->nraw++;
}

static long cmd_length(const Buf *d, int cmd, long ptr)
{
    if (cmd == 0x4F || cmd == 0x50) return 2;
    if (cmd >= 0x30 && cmd <= 0x3F) return 2;
    if (cmd >= 0x40 && cmd <= 0x4E) return 3;
    if (cmd >= 0x51 && cmd <= 0x5F) return 3;
    if (cmd == 0x67) return 7 + (long)rd32(d, ptr + 3);
    if (cmd == 0x68) return 12;
    if (cmd >= 0x90 && cmd <= 0x95) {
        static const int L[6] = { 5, 5, 6, 11, 2, 5 };
        return L[cmd - 0x90];
    }
    if (cmd >= 0xA0 && cmd <= 0xBF) return 3;
    if (cmd >= 0xC0 && cmd <= 0xDF) return 4;
    if (cmd >= 0xE0) return 5;
    return 1;
}

static int parse_vgm(const Buf *d, Info *in)
{
    long rel, data_offset, ptr, t = 0, pcm_pos = 0, n = d->n, g;
    if (n < 4 || memcmp(d->p, "Vgm ", 4) != 0) return 1;
    in->version = rd32(d, 8);
    rel = in->version >= 0x150 ? (long)rd32(d, 0x34) : 0;
    data_offset = rel ? 0x34 + rel : 0x40;
    in->sn_clock = rd32(d, 0x0C);
    if (in->version >= 0x110) {
        long k;
        in->n_snflags = 0;
        for (k = 0x28; k < 0x2C && k < n; k++) in->sn_flags[in->n_snflags++] = d->p[k];
    } else {
        in->sn_flags[0] = 9; in->sn_flags[1] = 0; in->sn_flags[2] = 0x10; in->sn_flags[3] = 0;
        in->n_snflags = 4;
    }
    in->ym_clock = (in->version >= 0x110 ? rd32(d, 0x2C) : rd32(d, 0x10)) & 0x3FFFFFFFUL;
    in->total = rd32(d, 0x18);
    in->loop_abs = rd32(d, 0x1C) ? 0x1C + (long)rd32(d, 0x1C) : 0;
    in->loop_t = -1;
    in->gd3_off = -1;
    g = (long)rd32(d, 0x14);
    if (g) {
        g += 0x14;
        if (g + 4 <= n && memcmp(d->p + g, "Gd3 ", 4) == 0) {
            long len = 12 + (long)rd32(d, g + 8);
            in->gd3_off = g;
            in->gd3_len = (g + len > n) ? n - g : len;
        }
    }
    if (in->ym_clock == 0) in->ym_clock = YMCLK_DEFAULT;
    if (in->n_snflags == 4 && !in->sn_flags[0] && !in->sn_flags[1] && !in->sn_flags[2] && !in->sn_flags[3]) {
        in->sn_flags[0] = 9; in->sn_flags[2] = 0x10;
    }
    ptr = data_offset;
    while (ptr < n) {
        int cmd;
        if (ptr == in->loop_abs && in->loop_abs) in->loop_t = t;
        cmd = d->p[ptr];
        if (cmd >= 0x80 && cmd <= 0x8F) {
            if (pcm_pos < in->bank.n) dac_add(in, t, pcm_pos, in->bank.p[pcm_pos]);
            pcm_pos++;
            t += cmd & 0x0F;
            ptr++;
        } else if (cmd >= 0x70 && cmd <= 0x7F) {
            t += (cmd & 0x0F) + 1; ptr++;
        } else if (cmd == 0x66) {
            break;
        } else if (cmd == 0x52 || cmd == 0x53) {
            int port = cmd - 0x52, reg, val;
            if (ptr + 2 >= n) return 1;
            reg = d->p[ptr + 1]; val = d->p[ptr + 2];
            if (port == 0 && reg == 0x2A) dac_add(in, t, -1, val);
            else ev_add(in, t, 0, port, reg, val);
            ptr += 3;
        } else if (cmd == 0x67) {
            long size;
            if (ptr + 7 > n) return 1;
            size = (long)rd32(d, ptr + 3);
            if (d->p[ptr + 2] == 0x00 && ptr + 7 < n) {
                long m = (ptr + 7 + size > n) ? n - (ptr + 7) : size;
                buf_add(&in->bank, d->p + ptr + 7, m);
            } else if (d->p[ptr + 2] == 0x01 || d->p[ptr + 2] == 0x02 ||
                       d->p[ptr + 2] == 0xC0 || d->p[ptr + 2] == 0xC1) {
                long len = 7 + (long)(size & 0x7FFFFFFFUL);
                raw_add(in, t, ptr, (ptr + len > n) ? n - ptr : len);
            }
            ptr += 7 + size;
        } else if (cmd == 0xE0) {
            if (ptr + 5 > n) return 1;
            pcm_pos = (long)rd32(d, ptr + 1);
            ptr += 5;
        } else if (cmd == 0x50) {
            if (ptr + 1 >= n) return 1;
            ev_add(in, t, 1, 0, 0, d->p[ptr + 1]);
            ptr += 2;
        } else if (cmd == 0x61) {
            if (ptr + 3 > n) return 1;
            t += d->p[ptr + 1] | (d->p[ptr + 2] << 8);
            ptr += 3;
        } else if (cmd == 0x62) { t += 735; ptr++; }
        else if (cmd == 0x63) { t += 882; ptr++; }
        else {
            long L = cmd_length(d, cmd, ptr);
            if (cmd == 0xA0) in->has_ay = 1;
            if (cmd == 0xA0 || cmd == 0xB0 || cmd == 0xB1 || cmd == 0xC1 || cmd == 0xC2 ||
                (cmd == 0x68 && ptr + 2 < n && (d->p[ptr + 2] == 0x01 || d->p[ptr + 2] == 0x02)))
                raw_add(in, t, ptr, (ptr + L > n) ? n - ptr : L);
            ptr += L;
        }
    }
    in->end_t = t;
    return 0;
}

static void key_burst(Info *in)
{
    long a = 0, b, i, j, n = 0;
    while (a < in->nev) {
        b = a;
        while (b < in->nev && in->ev[b].t == in->ev[a].t) b++;
        if (b - a > 8) {
            int ch;
            for (ch = 0; ch < 8; ch++) {
                long last = -1, off = -1, cnt = 0;
                for (i = b - 1; i >= a; i--) {
                    YEv *e = &in->ev[i];
                    if (e->psg || e->port || e->reg != 0x28 || (e->val & 7) != ch) continue;
                    cnt++;
                    if (last < 0) last = i;
                    else if (off < 0 && (in->ev[last].val & 0xF0) && !(e->val & 0xF0)) off = i;
                }
                if (cnt <= 2) continue;
                for (i = a; i < b; i++) {
                    YEv *e = &in->ev[i];
                    if (e->psg || e->port || e->reg != 0x28 || (e->val & 7) != ch) continue;
                    if (i != last && i != off) e->reg = 0xFF;
                }
            }
        }
        a = b;
    }
    for (j = 0; j < in->nev; j++)
        if (!(in->ev[j].reg == 0xFF && !in->ev[j].psg && in->ev[j].port == 0)) in->ev[n++] = in->ev[j];
    in->nev = n;
}

#define RAW_WIN 220L
static void raw_merge(Info *in, const Buf *d, Buf *mb)
{
    long i, j = 0, g_t = 0, g_hdr = -1, g_len = 0, g_addr = 0, g_idx = -1;
    for (i = 0; i < in->nraw; i++) {
        struct RawE e = in->raw[i];
        const unsigned char *p = d->p + e.off;
        int ok = (e.off >= 0 && e.len > 9 && e.len < 512 && p[0] == 0x67 && p[2] == 0xC1);
        long addr = ok ? (long)(p[7] | (p[8] << 8)) : 0;
        long dl = e.len - 9;
        if (ok && g_hdr >= 0 && addr >= g_addr && addr <= g_addr + g_len &&
            e.t - g_t < RAW_WIN && addr - g_addr + dl <= 4096) {
            long o = addr - g_addr, k;
            for (k = 0; k < dl && o + k < g_len; k++) mb->p[g_hdr + 9 + o + k] = p[9 + k];
            if (k < dl) { buf_add(mb, p + 9 + k, dl - k); g_len += dl - k; }
            continue;
        }
        if (g_hdr >= 0) {
            long sz = g_len + 2;
            mb->p[g_hdr + 3] = (unsigned char)sz; mb->p[g_hdr + 4] = (unsigned char)(sz >> 8);
            mb->p[g_hdr + 5] = (unsigned char)(sz >> 16); mb->p[g_hdr + 6] = 0;
            in->raw[g_idx].len = 9 + g_len;
            g_hdr = -1;
        }
        if (ok) {
            g_hdr = mb->n; g_t = e.t; g_addr = addr; g_len = dl; g_idx = j;
            buf_add(mb, p, e.len);
            in->raw[j].t = e.t; in->raw[j].off = -(g_hdr + 1); in->raw[j].len = e.len;
        } else
            in->raw[j] = e;
        j++;
    }
    if (g_hdr >= 0) {
        long sz = g_len + 2;
        mb->p[g_hdr + 3] = (unsigned char)sz; mb->p[g_hdr + 4] = (unsigned char)(sz >> 8);
        mb->p[g_hdr + 5] = (unsigned char)(sz >> 16); mb->p[g_hdr + 6] = 0;
        in->raw[g_idx].len = 9 + g_len;
    }
    in->nraw = j;
}

typedef struct { long t, off, cnt; double hz; } Run;

static void dac_streams(Info *in, Buf *out_bank, Run **pruns, long *pnr)
{
    long i = 0, n = in->ndac, nr = 0, cap = 256;
    Run *runs = (Run *)xmalloc(sizeof(Run) * (size_t)cap);
    long *ex_off = NULL, *ex_len = NULL, nex = 0;
    long *cl_src = NULL, *cl_cnt = NULL, *cl_off = NULL, *cl_len = NULL, ncl = 0;
    Buf data;
    buf_init(&data);
    out_bank->n = 0;
    buf_add(out_bank, in->bank.p, in->bank.n);
    while (i < n) {
        long j = i + 1, cnt, span, off = 0, k, dup;
        double hz;
        const DacW *dw = in->dacw;
        while (j < n) {
            long t0 = dw[j - 1].t, t1 = dw[j].t, p0 = dw[j - 1].pos, p1 = dw[j].pos;
            if (t1 - t0 > DAC_GAP_MAX) break;
            if ((p0 < 0) != (p1 < 0)) break;
            if (p0 >= 0 && p1 != p0 + 1) break;
            if (in->loop_t >= 0 && t0 < in->loop_t && in->loop_t <= t1) break;
            j++;
        }
        cnt = j - i;
        span = dw[j - 1].t - dw[i].t;
        dup = 0;
        for (k = i + 1; k < j; k++) if (dw[k].t == dw[k - 1].t) dup++;
        if (cnt - dup > 1 && span > 0) hz = 44100.0 * (double)(cnt - dup - 1) / (double)span;
        else if (cnt > 1 && span > 0) hz = 44100.0 * (double)(cnt - 1) / (double)span;
        else if (nr) hz = runs[nr - 1].hz;
        else hz = 8000.0;
        if (dw[i].pos >= 0 && dup) {
            long src = dw[i].pos, c0 = cnt;
            for (k = 0; k < ncl; k++) if (cl_src[k] == src && cl_cnt[k] == c0) break;
            if (k < ncl) { off = cl_off[k]; cnt = cl_len[k]; hz = 44100.0; }
            else if (out_bank->n + span + 1 > DAC_BANK_MAX) {
                off = src;
                hz = 44100.0 * (double)(cnt - 1) / (double)(span > 0 ? span : 1);
            }
            else {
                long t0 = dw[i].t, tt, kk = i;
                data.n = 0;
                for (tt = 0; t0 + tt <= dw[j - 1].t; tt++) {
                    while (kk + 1 < j && dw[kk + 1].t <= t0 + tt) kk++;
                    buf_byte(&data, (dw[kk].pos >= 0 && dw[kk].pos < in->bank.n) ? in->bank.p[dw[kk].pos] : 0x80);
                }
                hz = 44100.0;
                cl_src = (long *)xrealloc(cl_src, sizeof(long) * (size_t)(ncl + 1));
                cl_cnt = (long *)xrealloc(cl_cnt, sizeof(long) * (size_t)(ncl + 1));
                cl_off = (long *)xrealloc(cl_off, sizeof(long) * (size_t)(ncl + 1));
                cl_len = (long *)xrealloc(cl_len, sizeof(long) * (size_t)(ncl + 1));
                cl_src[ncl] = src; cl_cnt[ncl] = c0; cl_off[ncl] = out_bank->n; cl_len[ncl] = data.n; ncl++;
                off = out_bank->n; cnt = data.n;
                buf_add(out_bank, data.p, data.n);
            }
        } else if (dw[i].pos >= 0) off = dw[i].pos;
        else {
            data.n = 0;
            for (k = i; k < j; k++) buf_byte(&data, dw[k].v);
            for (k = 0; k < nex; k++)
                if (ex_len[k] == data.n && memcmp(out_bank->p + ex_off[k], data.p, (size_t)data.n) == 0) break;
            if (k < nex) off = ex_off[k];
            else {
                ex_off = (long *)xrealloc(ex_off, sizeof(long) * (size_t)(nex + 1));
                ex_len = (long *)xrealloc(ex_len, sizeof(long) * (size_t)(nex + 1));
                ex_off[nex] = out_bank->n; ex_len[nex] = data.n; nex++;
                off = out_bank->n;
                buf_add(out_bank, data.p, data.n);
            }
        }
        if (nr >= cap) { cap *= 2; runs = (Run *)xrealloc(runs, sizeof(Run) * (size_t)cap); }
        runs[nr].t = dw[i].t; runs[nr].off = off; runs[nr].cnt = cnt;
        runs[nr].hz = hz < 100.0 ? 100.0 : (hz > 44100.0 ? 44100.0 : hz);
        nr++;
        i = j;
    }
    free(ex_off); free(ex_len);
    free(cl_src); free(cl_cnt); free(cl_off); free(cl_len);
    buf_free(&data);
    *pruns = runs; *pnr = nr;
}

static void resample(const unsigned char *seg, long nx, double src_hz, double dst_hz, double gain, Buf *out)
{
    long n_out = (long)((double)nx * (double)dst_hz / src_hz) + 1, i, k;
    double fc = (dst_hz / src_hz < 1.0 ? dst_hz / src_hz : 1.0) * 0.92;
    long half = (long)ceil(8.0 / fc);
    double step = src_hz / dst_hz, pi = 3.141592653589793;
    for (i = 0; i < n_out; i++) {
        double t = (double)i * step, acc = 0.0;
        long k0 = (long)floor(t);
        int v;
        for (k = k0 - half + 1; k < k0 + half + 1; k++) {
            double dd, h, w;
            if (k < 0 || k >= nx) continue;
            dd = t - (double)k;
            if (dd == 0.0) h = fc;
            else {
                double a = pi * fc * dd;
                h = fc * sin(a) / a;
            }
            w = dd / (double)half;
            if (w <= -1.0 || w >= 1.0) continue;
            h *= 0.42 + 0.5 * cos(pi * w) + 0.08 * cos(2 * pi * w);
            acc += (double)((int)seg[k] - 128) * h;
        }
        v = (int)pyround(acc * gain) + 128;
        buf_byte(out, v < 0 ? 0 : (v > 255 ? 255 : v));
    }
}

typedef struct { long off, cnt, hz; } Rv;
static int cmp_rv(const void *a, const void *b)
{
    const Rv *x = (const Rv *)a, *y = (const Rv *)b;
    if (x->off != y->off) return x->off < y->off ? -1 : 1;
    if (x->hz != y->hz) return x->hz < y->hz ? -1 : 1;
    if (x->cnt != y->cnt) return x->cnt < y->cnt ? -1 : 1;
    return 0;
}

static int vdac_block(const Buf *bank, const Run *runs, long nr, long rate, Buf *blk)
{
    Rv *rv = (Rv *)xmalloc(sizeof(Rv) * (size_t)(nr + 1));
    long i, nv = 0, head;
    struct Var { long off, cnt, lo, hi; double hz; } *var;
    Buf data;
    long *tab;
    for (i = 0; i < nr; i++) { rv[i].off = runs[i].off; rv[i].cnt = runs[i].cnt; rv[i].hz = (long)pyround(runs[i].hz); }
    qsort(rv, (size_t)nr, sizeof(Rv), cmp_rv);
    var = (struct Var *)xmalloc(sizeof(*var) * (size_t)(nr + 1));
    i = 0;
    while (i < nr) {
        long off = rv[i].off, j = i;
        long c0 = -1;
        double hsum = 0; long hn = 0, lo = 0;
        while (j < nr && rv[j].off == off) {
            if (c0 >= 0 && (double)rv[j].hz <= (double)lo * (1.0 + VDAC_TOL)) {
                if (rv[j].cnt > var[c0].cnt) var[c0].cnt = rv[j].cnt;
                var[c0].hi = rv[j].hz;
                hsum += (double)rv[j].hz; hn++;
            } else {
                if (c0 >= 0) var[c0].hz = hsum / (double)hn;
                c0 = nv++;
                lo = rv[j].hz;
                var[c0].off = off; var[c0].cnt = rv[j].cnt; var[c0].lo = lo; var[c0].hi = lo;
                hsum = (double)lo; hn = 1;
            }
            j++;
        }
        if (c0 >= 0) var[c0].hz = hsum / (double)hn;
        i = j;
    }
    head = 10 + 24 * nv;
    buf_init(&data);
    tab = (long *)xmalloc(sizeof(long) * 6 * (size_t)(nv + 1));
    for (i = 0; i < nv; i++) {
        long s0 = var[i].off, sn = var[i].cnt, before = data.n;
        if (s0 > bank->n) s0 = bank->n;
        if (s0 + sn > bank->n) sn = bank->n - s0;
        if (sn < 0) sn = 0;
        resample(bank->p + s0, sn, var[i].hz, (double)rate, VDAC_GAIN, &data);
        tab[i * 6 + 0] = var[i].off; tab[i * 6 + 1] = var[i].cnt;
        tab[i * 6 + 2] = var[i].lo; tab[i * 6 + 3] = var[i].hi;
        tab[i * 6 + 4] = head + before; tab[i * 6 + 5] = data.n - before;
        if (head + data.n > VDAC_MAX) {
            free(rv); free(var); free(tab); buf_free(&data);
            return 0;
        }
    }
    blk->n = 0;
    buf_add(blk, "VDAC", 4);
    buf_u16(blk, 1); buf_u16(blk, (unsigned)rate); buf_u16(blk, (unsigned)nv);
    for (i = 0; i < nv * 6; i++) buf_u32(blk, (unsigned long)tab[i]);
    buf_add(blk, data.p, data.n);
    free(rv); free(var); free(tab); buf_free(&data);
    return 1;
}

typedef struct { int dt, mul, tl, ks, ar, am, d1r, d2r, sl, rr, ssg; } Op;

#define MAXU 8
typedef struct {
    int four, chans[2], ops[4], nops, cnt0, cnt1, fb, pitch, tladj[4];
} Unit;
typedef struct { Unit u[MAXU]; int n; } Plan;
typedef struct { int cc, which, yop, rr, delta; } RelE;
typedef struct { RelE e[MAXU * 4]; int n; } Rel;

typedef struct { long t, seq; unsigned char port, reg, val; } OplW;

typedef struct {
    double ymclk;
    unsigned char regs[2][256];
    int keyed[6];
    Plan plan[6];
    int has_plan[6];
    Rel rel[6];
    OplW *ev; long nev, capev;
    long seq;
    int conn4;
    long t;
    int dac_on;
    int shadow[2][256];
    int lfo_on;
    long lfo_start, lfo_next;
    double pm_cents[6];
    int am_steps[6];
    int free_pool[18], nfree;
    int kmask[6];
    int sd_on[6][5], sd_extra[6][5];
    double sd_speed[6][5];
    long sd_start[6][5];
} T;

static T *S;

static void w_raw(int port, int reg, int val)
{
    OplW *e;
    reg &= 0xFF; val &= 0xFF;
    if (S->shadow[port][reg] == val) return;
    S->shadow[port][reg] = val;
    if (S->nev >= S->capev) {
        S->capev = S->capev ? S->capev * 2 : 16384;
        S->ev = (OplW *)xrealloc(S->ev, sizeof(OplW) * (size_t)S->capev);
    }
    e = &S->ev[S->nev++];
    e->t = S->t; e->seq = S->seq++; e->port = (unsigned char)port;
    e->reg = (unsigned char)reg; e->val = (unsigned char)val;
}

static void wch(int chan, int reg, int val) { w_raw(chan >= 9 ? 1 : 0, reg, val); }

static void wop(int chan, int which, int base, int val)
{
    int c = chan % 9;
    wch(chan, base + OPOFF[c] + (which ? 3 : 0), val);
}

static Op ymop(int ch, int op)
{
    Op o;
    int port = ch / 3, c = ch % 3;
    const unsigned char *r = S->regs[port];
    int b = c + 4 * SLOTN[op];
    o.dt = (r[0x30 + b] >> 4) & 7; o.mul = r[0x30 + b] & 15; o.tl = r[0x40 + b] & 127;
    o.ks = r[0x50 + b] >> 6; o.ar = r[0x50 + b] & 31; o.am = r[0x60 + b] >> 7;
    o.ssg = r[0x90 + b] & 15;
    o.d1r = r[0x60 + b] & 31; o.d2r = r[0x70 + b] & 31; o.sl = r[0x80 + b] >> 4;
    o.rr = r[0x80 + b] & 15;
    return o;
}

static void ymch(int ch, int *alg, int *fb, int *pan, int *pms, int *ams)
{
    int port = ch / 3, c = ch % 3;
    const unsigned char *r = S->regs[port];
    *alg = r[0xB0 + c] & 7; *fb = (r[0xB0 + c] >> 3) & 7; *pan = r[0xB4 + c] >> 6;
    *pms = r[0xB4 + c] & 7; *ams = (r[0xB4 + c] >> 4) & 3;
}

static int special3(void) { return (S->regs[0][0x27] & 0xC0) != 0; }

static void op_block_fnum(int ch, int op, int *block, int *fnum)
{
    int port = ch / 3, c = ch % 3;
    const unsigned char *r = S->regs[port];
    if (ch == 2 && op != 4 && special3()) {
        int lo = (op == 1) ? 0xA9 : (op == 2) ? 0xAA : 0xA8;
        int hi = lo + 4;
        *block = (r[hi] >> 3) & 7; *fnum = ((r[hi] & 7) << 8) | r[lo];
        return;
    }
    *block = (r[0xA4 + c] >> 3) & 7; *fnum = ((r[0xA4 + c] & 7) << 8) | r[0xA0 + c];
}

static const unsigned char DET_ADJ[32][4] = {
    {0,0,1,2},{0,0,1,2},{0,0,1,2},{0,0,1,2},{0,1,2,2},{0,1,2,3},{0,1,2,3},{0,1,2,3},
    {0,1,2,4},{0,1,3,4},{0,1,3,4},{0,1,3,5},{0,2,4,5},{0,2,4,6},{0,2,4,6},{0,2,5,7},
    {0,2,5,8},{0,3,6,8},{0,3,6,9},{0,3,7,10},{0,4,8,11},{0,4,8,12},{0,4,9,13},{0,5,10,14},
    {0,5,11,16},{0,6,12,17},{0,6,13,19},{0,7,14,20},{0,8,16,22},{0,8,16,22},{0,8,16,22},{0,8,16,22} };
#ifdef DTON
static int g_use_dt = 1;
#else
static int g_use_dt = 0;
#endif
int g_md_ssgeg = 0;

static double op_hz(int ch, int op)
{
    int block, fnum;
    long inc;
    double fs, mulf;
    Op o = ymop(ch, op);
    op_block_fnum(ch, op, &block, &fnum);
    inc = ((long)fnum << block) >> 1;
    if (g_use_dt && (o.dt & 3)) {
        int d = DET_ADJ[ym_kc(block, fnum) & 31][o.dt & 3];
        inc += (o.dt & 4) ? -d : d;
    }
    fs = S->ymclk / 144.0;
    mulf = o.mul == 0 ? 0.5 : (double)o.mul;
    return (double)inc * fs / 1048576.0 * mulf;
}

static void opl_blk_fn(double hz, int *blk, int *fn)
{
    int b;
    for (b = 0; b < 8; b++) {
        long f = (long)pyround(hz * (double)(1L << (20 - b)) / ((double)OPLCLK / 288.0));
        if (f < 1024) { *blk = b; *fn = f < 0 ? 0 : (int)f; return; }
    }
    *blk = 7; *fn = 1023;
}

#define SSG_REP(o) (((o).ssg & 9) == 8 && ((o).d1r || (o).d2r))
#define SSG_ATT 14

static int g_sd_cur = 0;

static void rates(const Op *o, int kc_ym, int kc_opl, int *oksr, int *oar, int *odr, int *osrr, int *orr, int *oegt)
{
    int ksoff = kc_ym >> (3 - o->ks);
    int t_ar, t_dr, t_d2, t_rr, ksr;
    double best_err = -1;
    int b_ksr = 0, b_ar = 0, b_dr = 0, b_d2 = 0, b_rr = 0;
#define EFF(v) ((v) == 0 ? -1 : ((v) + ksoff > 63 ? 63 : (v) + ksoff))
    t_ar = EFF(2 * o->ar); t_dr = EFF(2 * o->d1r); t_d2 = EFF(2 * o->d2r);
#undef EFF
    t_rr = 4 * o->rr + 2 + ksoff; if (t_rr > 63) t_rr = 63;
    if (g_sd_cur) t_d2 = -1;
    if (SSG_REP(*o)) { t_dr = -1; t_d2 = -1; }
    for (ksr = 0; ksr < 2; ksr++) {
        int off = ksr ? kc_opl : kc_opl >> 2;
        int v[4], tg[4], k;
        double e[4], err;
        tg[0] = t_ar; tg[1] = t_dr; tg[2] = t_d2; tg[3] = t_rr;
        for (k = 0; k < 4; k++) {
            if (tg[k] < 0) { v[k] = 0; e[k] = 0.0; }
            else {
                double tgt = (double)tg[k] - EG_OFFSET;
                int r = (int)pyround((tgt - (double)off) / 4.0);
                if (r < 1) r = 1;
                if (r > 15) r = 15;
                v[k] = r;
                e[k] = fabs(4.0 * r + off - tgt);
            }
        }
        if (o->ar >= 31) { v[0] = 15; e[0] = 0.0; }
        err = e[0] + e[1] + e[2] + e[3];
        if (best_err < 0 || err < best_err) {
            best_err = err; b_ksr = ksr; b_ar = v[0]; b_dr = v[1]; b_d2 = v[2]; b_rr = v[3];
        }
    }
    *oksr = b_ksr; *oar = b_ar; *odr = b_dr; *orr = b_rr;
    if (o->d2r && !g_sd_cur && !SSG_REP(*o)) { *oegt = 0; *osrr = b_d2; }
    else { *oegt = 1; *osrr = b_rr; }
}

typedef struct { int chain[4], n, tladj[4], fb; } Voice;

static void voice_set(Voice *v, const int *chain, int n, int fb)
{
    int i;
    for (i = 0; i < n; i++) v->chain[i] = chain[i];
    v->n = n; v->fb = fb;
    for (i = 0; i < 4; i++) v->tladj[i] = 0;
}

static double ch_hz(int ch, int op)
{
    Op o = ymop(ch, op);
    return op_hz(ch, op) / OPL_MULT[MUL_MAP[o.mul]];
}

typedef struct { int n, v[2]; } Grp;

static int op_keyed(int ch, int op)
{
    if (op < 1 || op > 4 || !S->kmask[ch]) return 1;
    return (S->kmask[ch] >> (3 + op)) & 1;
}

static void make_plan(int ch, Plan *pl)
{
    int alg, fb, pan, pms, ams, A, B, Sp, i, j;
    int chans2[24], nch2 = 0, free2[24], nfree2;
    Voice voices[4];
    int nv = 0;
    Voice *singles[4], *pairs2[4];
    int ns = 0, np = 0;
    Grp merged[8];
    int nm = 0;
    struct { int pair; int idx; Grp g; } slots[12];
    int nsl = 0;
    ymch(ch, &alg, &fb, &pan, &pms, &ams);
    A = PAIRS[ch][0]; B = PAIRS[ch][1]; Sp = SPARE[ch];
    chans2[nch2++] = A; chans2[nch2++] = B; chans2[nch2++] = Sp;
    if (ch == 2 && special3())
        for (i = 0; i < S->nfree; i++) chans2[nch2++] = S->free_pool[i];
    if (alg == 1 || alg == 2 || alg == 3) {
        static const int c14[2] = { 1, 4 }, c234[3] = { 2, 3, 4 }, c124[3] = { 1, 2, 4 }, c34[2] = { 3, 4 };
        static const int c134[3] = { 1, 3, 4 }, c1234[4] = { 1, 2, 3, 4 };
        int dual = 0;
        if (alg == 2 && !op_keyed(ch, 1)) {
            voice_set(&voices[nv++], c234, 3, 0);
        } else if (alg == 2 && !op_keyed(ch, 3)) {
            voice_set(&voices[nv++], c14, 2, fb);
        } else if (alg == 2) {
            if (ymop(ch, 3).tl - ymop(ch, 1).tl >= 12) {
                voice_set(&voices[nv++], c14, 2, fb);
            } else {
                voice_set(&voices[nv], c14, 2, fb); voices[nv].tladj[1] = g_dual_adj; nv++;
                voice_set(&voices[nv], c234, 3, 0); voices[nv].tladj[2] = g_dual_adj; nv++;
                dual = 1;
            }
        } else if (alg == 3) {
            voice_set(&voices[nv], c124, 3, fb); voices[nv].tladj[2] = g_dual_adj; nv++;
            voice_set(&voices[nv], c34, 2, 0); voices[nv].tladj[1] = g_dual_adj; nv++;
        } else {
            int dd = ymop(ch, 2).tl - ymop(ch, 1).tl;
            if (dd >= 4) voice_set(&voices[nv++], c134, 3, fb);
            else if (dd <= -4) voice_set(&voices[nv++], c234, 3, 0);
            else voice_set(&voices[nv++], c1234, 4, fb);
        }
        if (dual && DUAL_BOOST)
            for (j = 0; j < nv; j++)
                for (i = 0; i < voices[j].n - 1; i++) voices[j].tladj[i] -= DUAL_BOOST;
    } else if (alg == 0) {
        static const int c[4] = { 1, 2, 3, 4 };
        voice_set(&voices[nv++], c, 4, fb);
    } else if (alg == 4) {
        static const int a[2] = { 1, 2 }, b[2] = { 3, 4 };
        voice_set(&voices[nv++], a, 2, fb); voice_set(&voices[nv++], b, 2, 0);
    } else if (alg == 5) {
        static const int a[2] = { 1, 2 }, b[2] = { 1, 3 }, c[2] = { 1, 4 };
        voice_set(&voices[nv++], a, 2, fb); voice_set(&voices[nv++], b, 2, fb);
        voice_set(&voices[nv++], c, 2, fb);
    } else if (alg == 6) {
        static const int a[2] = { 1, 2 }, b[1] = { 3 }, c[1] = { 4 };
        voice_set(&voices[nv++], a, 2, fb); voice_set(&voices[nv++], b, 1, 0);
        voice_set(&voices[nv++], c, 1, 0);
    } else {
        static const int a[1] = { 1 }, b[1] = { 2 }, c[1] = { 3 }, d[1] = { 4 };
        voice_set(&voices[nv++], a, 1, fb); voice_set(&voices[nv++], b, 1, 0);
        voice_set(&voices[nv++], c, 1, 0); voice_set(&voices[nv++], d, 1, 0);
    }
    for (j = 0; j < nv; j++)
        if (voices[j].fb >= 6 && voices[j].n >= 2)
            voices[j].tladj[voices[j].n - 1] -= (voices[j].fb == 7) ? 3 : 1;
    pl->n = 0;
    nfree2 = nch2;
    for (i = 0; i < nch2; i++) free2[i] = chans2[i];
    for (j = 0; j < nv; j++) {
        Voice *v = &voices[j];
        if (v->n >= 3) {
            Unit *u = &pl->u[pl->n++];
            int k, m = 0;
            memset(u, 0, sizeof(*u));
            u->four = 1; u->chans[0] = A; u->chans[1] = B; u->nops = 4;
            for (k = 0; k < 4 - v->n; k++) u->ops[k] = NONE;
            for (k = 0; k < v->n; k++) u->ops[4 - v->n + k] = v->chain[k];
            for (k = 0; k < v->n; k++) u->tladj[k + 4 - v->n] = v->tladj[k];
            u->fb = v->fb; u->pitch = v->chain[v->n - 1];
            for (k = 0; k < nfree2; k++) if (free2[k] != A && free2[k] != B) free2[m++] = free2[k];
            nfree2 = m;
        }
    }
    for (j = 0; j < nv; j++) {
        if (voices[j].n == 1) singles[ns++] = &voices[j];
        else if (voices[j].n == 2) pairs2[np++] = &voices[j];
    }
    {
        int used[4] = { 0, 0, 0, 0 };
        for (i = 0; i < ns; i++) {
            double hi;
            if (used[i]) continue;
            merged[nm].n = 1; merged[nm].v[0] = i;
            used[i] = 1;
            hi = ch_hz(ch, singles[i]->chain[0]);
            for (j = i + 1; j < ns; j++) {
                double hj;
                if (used[j] || merged[nm].n >= 2) continue;
                hj = ch_hz(ch, singles[j]->chain[0]);
                if (fabs(hi - hj) <= 1e-4 * (hi > 1.0 ? hi : 1.0)) {
                    merged[nm].v[merged[nm].n++] = j;
                    used[j] = 1;
                }
            }
            nm++;
        }
    }
    for (;;) {
        int ones[8], no = 0, bi = -1, bj = -1, k;
        double bd = 0;
        Grp g;
        for (k = 0; k < nm; k++) if (merged[k].n == 1) ones[no++] = k;
        if (!(np + nm > nfree2 && no >= 2)) break;
        for (i = 0; i < no; i++)
            for (j = i + 1; j < no; j++) {
                double d = fabs(log((op_hz(ch, singles[merged[ones[i]].v[0]]->chain[0]) + 1) /
                                    (op_hz(ch, singles[merged[ones[j]].v[0]]->chain[0]) + 1)));
                if (bi < 0 || d < bd) { bd = d; bi = ones[i]; bj = ones[j]; }
            }
        g.n = 2; g.v[0] = merged[bi].v[0]; g.v[1] = merged[bj].v[0];
        {
            int m = 0;
            for (k = 0; k < nm; k++) if (k != bi && k != bj) merged[m++] = merged[k];
            nm = m;
        }
        merged[nm++] = g;
    }
    for (i = 0; i < np; i++) { slots[nsl].pair = 1; slots[nsl].idx = i; nsl++; }
    for (i = 0; i < nm; i++) { slots[nsl].pair = 0; slots[nsl].g = merged[i]; nsl++; }
    while (nsl > nfree2) {
        int bestk = 0, bestl = -1, k;
        for (k = 0; k < nsl; k++) {
            int l;
            if (slots[k].pair) {
                Voice *v = pairs2[slots[k].idx];
                l = ymop(ch, v->chain[v->n - 1]).tl;
            } else {
                int q;
                l = 1000;
                for (q = 0; q < slots[k].g.n; q++) {
                    int tl = ymop(ch, singles[slots[k].g.v[q]]->chain[0]).tl;
                    if (tl < l) l = tl;
                }
            }
            if (l > bestl) { bestl = l; bestk = k; }
        }
        for (k = bestk; k < nsl - 1; k++) slots[k] = slots[k + 1];
        nsl--;
    }
    for (i = 0; i < nsl && i < nfree2; i++) {
        Unit *u = &pl->u[pl->n++];
        memset(u, 0, sizeof(*u));
        u->chans[0] = free2[i]; u->nops = 2;
        if (slots[i].pair) {
            Voice *v = pairs2[slots[i].idx];
            u->ops[0] = v->chain[0]; u->ops[1] = v->chain[1]; u->cnt0 = 0; u->fb = v->fb;
            u->pitch = v->chain[1];
            u->tladj[0] = v->tladj[0]; u->tladj[1] = v->tladj[1];
        } else if (slots[i].g.n == 2) {
            int a = singles[slots[i].g.v[0]]->chain[0], b = singles[slots[i].g.v[1]]->chain[0];
            if (b == 1) { int x = a; a = b; b = x; }
            u->ops[0] = a; u->ops[1] = b; u->cnt0 = 1; u->fb = (a == 1) ? fb : 0; u->pitch = b;
        } else {
            int x = singles[slots[i].g.v[0]]->chain[0];
            if (x == 1) { u->ops[0] = 1; u->ops[1] = NONE; u->cnt0 = 1; u->fb = fb; u->pitch = 1; }
            else { u->ops[0] = NONE; u->ops[1] = x; u->cnt0 = 0; u->fb = 0; u->pitch = x; }
        }
    }
}

#define ALIAS_HZ 2500.0
#define HIHAT_BOOST 2
static double unit_hz(int ch, const Unit *u)
{
    double hz = ch_hz(ch, u->pitch);
    if (S->pm_cents[ch] != 0.0) hz *= pow(2.0, S->pm_cents[ch] / 1200.0);
    if (hz > ALIAS_HZ && u->nops >= 2)
        hz *= ((double)OPLCLK / 288.0) / (S->ymclk / 144.0);
    return hz;
}

static int bass_boost(double hz)
{
    double b;
    if (hz <= 0) return 0;
    if (hz <= 80.0) return 6;
    if (hz >= 320.0) return 0;
    b = 6.0 * (1.0 - log(hz / 80.0) / log(4.0));
    return (int)(b + 0.5);
}

static int tl_value(int ch, int yop, int delta)
{
    Op o = ymop(ch, yop);
    int tl = o.tl + delta;
    if (!op_keyed(ch, yop)) return 63;
    if (o.am && S->am_steps[ch]) tl += S->am_steps[ch];
    if (SSG_REP(o)) tl += SSG_ATT;
    if (tl < 0) tl = 0;
    if (yop >= 1 && yop <= 4) tl += S->sd_extra[ch][yop];
    return tl > 63 ? 63 : tl;
}

static int opl_fb(int ch, const Unit *u)
{
    if (u->fb == 7 && ch_hz(ch, u->pitch) <= ALIAS_HZ) return 6;
    return u->fb;
}

static void update_pan(int ch)
{
    int alg, fb, pan, pms, ams, panbits, i;
    const Plan *pl = &S->plan[ch];
    ymch(ch, &alg, &fb, &pan, &pms, &ams);
    panbits = (((pan >> 1) & 1) << 4) | ((pan & 1) << 5);
    for (i = 0; i < pl->n; i++) {
        const Unit *u = &pl->u[i];
        if (u->four) {
            int a = u->chans[0], b = u->chans[1];
            wch(a, 0xC0 + a % 9, panbits | (opl_fb(ch, u) << 1) | u->cnt0);
            wch(b, 0xC0 + b % 9, panbits | u->cnt1);
        } else {
            int a = u->chans[0];
            wch(a, 0xC0 + a % 9, panbits | (opl_fb(ch, u) << 1) | u->cnt0);
        }
    }
}

static void apply_plan(int ch, const Plan *pl, Rel *rel)
{
    int alg, fb, pan, pms, ams, panbits, newconn, i, pi;
    ymch(ch, &alg, &fb, &pan, &pms, &ams);
    panbits = (((pan >> 1) & 1) << 4) | ((pan & 1) << 5);
    newconn = S->conn4 & ~(1 << ch);
    for (i = 0; i < pl->n; i++)
        if (pl->u[i].four)
            for (pi = 0; pi < 6; pi++)
                if (PAIRS[pi][0] == pl->u[i].chans[0] && PAIRS[pi][1] == pl->u[i].chans[1]) { newconn |= 1 << pi; break; }
    for (i = 0; i < pl->n; i++)
        if (!pl->u[i].four)
            for (pi = 0; pi < 6; pi++)
                if (pl->u[i].chans[0] == PAIRS[pi][0] || pl->u[i].chans[0] == PAIRS[pi][1]) newconn &= ~(1 << pi);
    if (newconn != S->conn4) {
        S->conn4 = newconn;
        w_raw(1, 0x04, S->conn4);
    }
    rel->n = 0;
    for (i = 0; i < pl->n; i++) {
        const Unit *u = &pl->u[i];
        int blk, fn, kc_opl, pos, slc[4], slw[4], ns, bboost;
        bboost = bass_boost(op_hz(ch, u->pitch));
        if (u->fb >= 6) bboost = 0;
        if (ch_hz(ch, u->pitch) > ALIAS_HZ && u->nops >= 2) bboost += HIHAT_BOOST;
        opl_blk_fn(unit_hz(ch, u), &blk, &fn);
        kc_opl = blk * 2 + ((fn >> 9) & 1);
        if (u->four) {
            slc[0] = u->chans[0]; slw[0] = 0; slc[1] = u->chans[0]; slw[1] = 1;
            slc[2] = u->chans[1]; slw[2] = 0; slc[3] = u->chans[1]; slw[3] = 1; ns = 4;
        } else {
            slc[0] = u->chans[0]; slw[0] = 0; slc[1] = u->chans[0]; slw[1] = 1; ns = 2;
        }
        for (pos = 0; pos < ns && pos < u->nops; pos++) {
            int yop = u->ops[pos], kc_ym, tl, ksr, ar, dr, srr, rrr, egt;
            int cc = slc[pos], which = slw[pos];
            int is_car = u->four ? (pos == 3) : (pos == 1 || u->cnt0 == 1);
            Op o;
            if (yop == NONE) {
                o.mul = 1; o.tl = 63; o.ks = 0; o.ar = 31; o.d1r = 0; o.d2r = 0; o.sl = 0; o.rr = 15;
                o.am = 0; o.dt = 0; o.ssg = 0;
                kc_ym = 0; tl = 63;
            } else {
                int by, fy;
                o = ymop(ch, yop);
                op_block_fnum(ch, yop, &by, &fy);
                kc_ym = ym_kc(by, fy);
                tl = tl_value(ch, yop, u->tladj[pos] - (is_car ? g_car_boost + bboost : 0));
            }
            g_sd_cur = (yop != NONE && S->sd_on[ch][yop]);
            rates(&o, kc_ym, kc_opl, &ksr, &ar, &dr, &srr, &rrr, &egt);
            g_sd_cur = 0;
            wop(cc, which, 0x20, (egt << 5) | (ksr << 4) | MUL_MAP[o.mul]);
            wop(cc, which, 0x40, tl);
            wop(cc, which, 0x60, (ar << 4) | dr);
            wop(cc, which, 0x80, (o.sl << 4) | srr);
            wop(cc, which, 0xE0, 0);
            rel->e[rel->n].cc = cc; rel->e[rel->n].which = which; rel->e[rel->n].yop = yop;
            rel->e[rel->n].rr = rrr;
            rel->e[rel->n].delta = u->tladj[pos] - ((yop != NONE && is_car) ? g_car_boost + bboost : 0);
            rel->n++;
        }
        if (u->four) {
            int a = u->chans[0], b = u->chans[1];
            wch(a, 0xC0 + a % 9, panbits | (opl_fb(ch, u) << 1) | u->cnt0);
            wch(b, 0xC0 + b % 9, panbits | u->cnt1);
        } else {
            int a = u->chans[0];
            wch(a, 0xC0 + a % 9, panbits | (opl_fb(ch, u) << 1) | u->cnt0);
        }
    }
}

static void set_freq(int ch, int key)
{
    int i;
    if (!S->has_plan[ch]) return;
    for (i = 0; i < S->plan[ch].n; i++) {
        const Unit *u = &S->plan[ch].u[i];
        int c = u->chans[0], blk, fn;
        opl_blk_fn(unit_hz(ch, u), &blk, &fn);
        wch(c, 0xA0 + c % 9, fn & 255);
        wch(c, 0xB0 + c % 9, (key ? 0x20 : 0) | (blk << 2) | (fn >> 8));
    }
}

static void refresh_tl(int ch)
{
    int i;
    for (i = 0; i < S->rel[ch].n; i++) {
        const RelE *e = &S->rel[ch].e[i];
        if (e->yop != NONE) wop(e->cc, e->which, 0x40, tl_value(ch, e->yop, e->delta));
    }
}

static double opn_speed(int r)
{
    if (r > 63) r = 63;
    return 7.5 * pow(2.0, (r - 16) / 4.0);
}

#define SD_MIN 2.7

static void sd_keyon(int ch)
{
    int yop;
    for (yop = 1; yop <= 4; yop++) {
        Op o = ymop(ch, yop);
        int by, fy, ksoff;
        double sp;
        S->sd_on[ch][yop] = 0; S->sd_extra[ch][yop] = 0;
        if (!o.d2r || SSG_REP(o)) continue;
        op_block_fnum(ch, yop, &by, &fy);
        ksoff = ym_kc(by, fy) >> (3 - o.ks);
        sp = opn_speed(2 * o.d2r + ksoff);
        if (sp >= SD_MIN) continue;
        if (o.sl == 0) S->sd_start[ch][yop] = S->t;
        else {
            double sl_db = (o.sl == 15) ? 93.0 : o.sl * 3.0;
            if (!o.d1r) continue;
            S->sd_start[ch][yop] = S->t + (long)(sl_db / opn_speed(2 * o.d1r + ksoff) * 44100.0);
        }
        S->sd_on[ch][yop] = 1; S->sd_speed[ch][yop] = sp;
    }
}

static void sd_advance(long t_until)
{
    int ch, yop;
    long saved_t = S->t;
    for (ch = 0; ch < 6; ch++) {
        if (!S->keyed[ch] || !S->has_plan[ch] || !S->plan[ch].n) continue;
        for (;;) {
            long best = -1; int by_op = 0;
            for (yop = 1; yop <= 4; yop++) {
                long tk;
                if (!S->sd_on[ch][yop] || S->sd_extra[ch][yop] >= 63) continue;
                tk = S->sd_start[ch][yop] +
                     (long)((S->sd_extra[ch][yop] + 1) * 0.75 / S->sd_speed[ch][yop] * 44100.0);
                if (tk <= t_until && (best < 0 || tk < best)) { best = tk; by_op = yop; }
            }
            if (best < 0) break;
            S->t = best;
            S->sd_extra[ch][by_op]++;
            refresh_tl(ch);
        }
    }
    S->t = saved_t;
}

static void lfo_advance(long t_until)
{
    int sps, ch;
    double fs, hz;
    long step, saved_t;
    if (!S->lfo_on) return;
    sps = LFO_SPS[S->regs[0][0x22] & 7];
    fs = S->ymclk / 144.0;
    hz = fs / (sps * 128.0);
    step = (long)(44100.0 / (hz * 16));
    if (step > LFO_UPDATE) step = LFO_UPDATE;
    if (step < 40) step = 40;
    saved_t = S->t;
    while (S->lfo_next <= t_until) {
        double phase, pm, am;
        S->t = S->lfo_next;
        phase = fmod((double)(S->lfo_next - S->lfo_start) * hz / 44100.0, 1.0);
        pm = sin(2 * 3.141592653589793 * phase);
        am = fabs(1.0 - 2.0 * phase);
        for (ch = 0; ch < 6; ch++) {
            int alg, fb, pan, pms, ams;
            if (!S->keyed[ch] || !S->has_plan[ch] || !S->plan[ch].n) continue;
            ymch(ch, &alg, &fb, &pan, &pms, &ams);
            if (pms) {
                S->pm_cents[ch] = PMS_CENTS[pms] * pm;
                set_freq(ch, 1);
            }
            if (ams) {
                int steps_eg = ((int)(126 * am)) >> AMS_SHIFT[ams];
                S->am_steps[ch] = (int)pyround(steps_eg / 8.0);
                refresh_tl(ch);
            }
        }
        S->lfo_next += step;
    }
    S->t = saved_t;
}

static int set_build(const int *v, int n, int *tab)
{
    int k, i, cnt = 0;
    for (i = 0; i < 8; i++) tab[i] = -1;
    for (k = 0; k < n; k++) {
        i = v[k] & 7;
        for (;;) {
            if (tab[i] == -1) { tab[i] = v[k]; cnt++; break; }
            if (tab[i] == v[k]) break;
            i = (i * 5 + 1) & 7;
        }
    }
    return cnt;
}

static void ym_write(int port, int reg, int val)
{
    int c, ch, k2;
    if (port == 0 && reg == 0x28) {
        int on;
        if ((val & 3) == 3) return;
        ch = (val & 3) + ((val & 4) ? 3 : 0);
        if (ch == 5 && S->dac_on) return;
        on = (val & 0xF0) != 0;
        if (on && S->keyed[ch] && S->kmask[ch] == (val & 0xF0)) return;
        S->kmask[ch] = on ? (val & 0xF0) : 0;
        if (on) {
            int retrig = S->keyed[ch], had_old = S->has_plan[ch] && S->plan[ch].n;
            Plan old = S->plan[ch];
            if (retrig) { set_freq(ch, 0); S->t += 1; }
            if (!S->lfo_on) S->pm_cents[ch] = 0.0;
            make_plan(ch, &S->plan[ch]);
            S->has_plan[ch] = 1;
            sd_keyon(ch);
            if (had_old) {
                int a[MAXU], b[MAXU], ta[8], tb[8], tr[8], i, k, na = 0, nb = 0, nr = 0;
                for (i = 0; i < old.n; i++) a[na++] = old.u[i].chans[0];
                for (i = 0; i < S->plan[ch].n; i++) b[nb++] = S->plan[ch].u[i].chans[0];
                set_build(a, na, ta);
                set_build(b, nb, tb);
                {
                    int dv[8];
                    for (i = 0; i < 8; i++) {
                        int in_b = 0;
                        if (ta[i] < 0) continue;
                        for (k = 0; k < 8; k++) if (tb[k] == ta[i]) in_b = 1;
                        if (!in_b) dv[nr++] = ta[i];
                    }
                    set_build(dv, nr, tr);
                }
                for (i = 0; i < 8; i++)
                    if (tr[i] >= 0) wch(tr[i], 0xB0 + tr[i] % 9, 0);
            }
            apply_plan(ch, &S->plan[ch], &S->rel[ch]);
            set_freq(ch, 1);
            if (retrig) S->t -= 1;
            S->keyed[ch] = 1;
        } else {
            if (S->keyed[ch] && S->has_plan[ch] && S->plan[ch].n) {
                int i;
                for (i = 0; i < S->rel[ch].n; i++) {
                    const RelE *e = &S->rel[ch].e[i];
                    if (e->yop != NONE) {
                        Op o = ymop(ch, e->yop);
                        wop(e->cc, e->which, 0x80, (o.sl << 4) | e->rr);
                    }
                }
                set_freq(ch, 0);
            }
            for (k2 = 1; k2 <= 4; k2++) S->sd_on[ch][k2] = 0;
            S->keyed[ch] = 0;
        }
        return;
    }
    S->regs[port][reg] = (unsigned char)val;
    if (reg >= 0x90 && reg <= 0x9F && (val & 8)) g_md_ssgeg = 1;
    if (port == 0 && reg < 0x30) {
        if (reg == 0x2B) {
            S->dac_on = (val & 0x80) != 0;
            if (S->dac_on && S->keyed[5]) ym_write(0, 0x28, 0x06);
        } else if (reg == 0x22) {
            int on = (val & 8) != 0;
            if (on && !S->lfo_on) { S->lfo_start = S->t; S->lfo_next = S->t; }
            if (!on && S->lfo_on) {
                int k;
                for (k = 0; k < 6; k++) { S->pm_cents[k] = 0.0; S->am_steps[k] = 0; }
                for (k = 0; k < 6; k++)
                    if (S->has_plan[k] && S->plan[k].n) { set_freq(k, S->keyed[k]); refresh_tl(k); }
            }
            S->lfo_on = on;
        } else if (reg == 0x27 && S->has_plan[2] && S->plan[2].n) {
            set_freq(2, S->keyed[2]);
        }
        return;
    }
    if (port == 0 && reg >= 0xA8 && reg <= 0xAA) {
        if (S->has_plan[2] && S->plan[2].n && special3()) set_freq(2, S->keyed[2]);
        return;
    }
    c = reg & 3;
    if (c == 3) return;
    ch = port * 3 + c;
    if (reg >= 0xB4 && reg <= 0xB6) {
        if (S->has_plan[ch] && S->plan[ch].n) update_pan(ch);
    } else if (reg >= 0xA0 && reg <= 0xA2) {
        if (S->has_plan[ch] && S->plan[ch].n) set_freq(ch, S->keyed[ch]);
    } else if (reg >= 0x40 && reg <= 0x4F && S->has_plan[ch] && S->plan[ch].n) {
        static const int ymap[4] = { 1, 3, 2, 4 };
        int yop = ymap[(reg - 0x40) >> 2], i;
        for (i = 0; i < S->rel[ch].n; i++)
            if (S->rel[ch].e[i].yop == yop)
                wop(S->rel[ch].e[i].cc, S->rel[ch].e[i].which, 0x40, tl_value(ch, yop, S->rel[ch].e[i].delta));
    }
}

enum { K_OPL, K_PSG, K_YMRAW, K_DAC, K_RAW };
typedef struct { long t, seq; unsigned char kind, a, b, c; long run; } SItem;

static int cmp_sitem(const void *x, const void *y)
{
    const SItem *a = (const SItem *)x, *b = (const SItem *)y;
    if (a->t != b->t) return a->t < b->t ? -1 : 1;
    return (a->seq > b->seq) - (a->seq < b->seq);
}

static void vwait(Buf *v, long samples)
{
    while (samples > 0) {
        if (samples <= 16) { buf_byte(v, (int)(0x70 + samples - 1)); samples = 0; }
        else if (samples == 735) { buf_byte(v, 0x62); samples = 0; }
        else if (samples == 882) { buf_byte(v, 0x63); samples = 0; }
        else {
            long ch = samples < 65535 ? samples : 65535;
            buf_byte(v, 0x61); buf_u16(v, (unsigned)ch);
            samples -= ch;
        }
    }
}

static void v3(Buf *v, int a, int b, int c) { buf_byte(v, a); buf_byte(v, b & 0xFF); buf_byte(v, c & 0xFF); }

int md_convertir(const Buf *d, Buf *res, char *msg)
{
    Info in;
    Buf rawm;
    int last_raw[2] = { -1, -1 };
    Buf v, dac_bank, blk;
    Run *runs = NULL;
    long nr = 0, i, cur = 0, loop_pos = -1, end_t, last_hz = -1;
    SItem *st = NULL;
    long nst = 0, capst = 0;
    int has_ym = 0, dac, used[6], c, have_dac = 0, k;
    memset(&in, 0, sizeof(in));
    buf_init(&in.bank);
    msg[0] = 0;
    g_md_ssgeg = 0;
    if (parse_vgm(d, &in)) {
        free(in.ev); free(in.dacw); free(in.raw); buf_free(&in.bank);
        strcpy(msg, "VGM danado");
        return -1;
    }
    key_burst(&in);
    g_car_boost = CAR_BOOST;
    for (i = 0; i < in.nev; i++) if (!in.ev[i].psg) { has_ym = 1; break; }
    if (!has_ym) {
        free(in.ev); free(in.dacw); free(in.raw); buf_free(&in.bank);
        return 0;
    }
    S = (T *)xmalloc(sizeof(T));
    memset(S, 0, sizeof(T));
    S->ymclk = (double)in.ym_clock;
    for (c = 0; c < 3; c++) { S->regs[0][0xB4 + c] = 0xC0; S->regs[1][0xB4 + c] = 0xC0; }
    for (c = 0; c < 256; c++) { S->shadow[0][c] = -1; S->shadow[1][c] = -1; }
    buf_init(&v); buf_init(&dac_bank); buf_init(&blk);
    {
        static const int ini[4][3] = { {1, 0x05, 1}, {1, 0x04, 0}, {0, 0x01, 0x20}, {0, 0xBD, 0} };
        for (k = 0; k < 4; k++) {
            v3(&v, ini[k][0] ? 0x5F : 0x5E, ini[k][1], ini[k][2]);
            S->shadow[ini[k][0]][ini[k][1]] = ini[k][2];
        }
    }
    for (c = 0; c < 18; c++) {
        int port = c >= 9 ? 1 : 0, cc = c % 9, q;
        for (q = 0; q < 2; q++) {
            int o = OPOFF[cc] + (q ? 3 : 0);
            v3(&v, port ? 0x5F : 0x5E, 0x40 + o, 63);
            v3(&v, port ? 0x5F : 0x5E, 0x60 + o, 0xFF);
            v3(&v, port ? 0x5F : 0x5E, 0x80 + o, 0x0F);
            S->shadow[port][0x40 + o] = 63;
            S->shadow[port][0x60 + o] = 0xFF;
            S->shadow[port][0x80 + o] = 0x0F;
        }
    }
    dac = 0;
    for (c = 0; c < 6; c++) used[c] = 0;
    for (i = 0; i < in.nev; i++) {
        const YEv *e = &in.ev[i];
        if (!e->psg && e->port == 0) {
            if (e->reg == 0x2B) dac = (e->val & 0x80) != 0;
            else if (e->reg == 0x28 && (e->val & 0xF0) && (e->val & 3) != 3) {
                c = (e->val & 3) + ((e->val & 4) ? 3 : 0);
                if (!(c == 5 && dac)) used[c] = 1;
            }
        }
    }
    for (c = 0; c < 6; c++)
        if (!used[c] && c != 2) {
            S->free_pool[S->nfree++] = PAIRS[c][0];
            S->free_pool[S->nfree++] = PAIRS[c][1];
            S->free_pool[S->nfree++] = SPARE[c];
        }
#define ST_ADD(tt, kk, aa, bb, cc2, rr) do { \
        if (nst >= capst) { capst = capst ? capst * 2 : 16384; st = (SItem *)xrealloc(st, sizeof(SItem) * (size_t)capst); } \
        st[nst].t = (tt); st[nst].seq = S->seq++; st[nst].kind = (unsigned char)(kk); \
        st[nst].a = (unsigned char)(aa); st[nst].b = (unsigned char)(bb); st[nst].c = (unsigned char)(cc2); \
        st[nst].run = (rr); nst++; } while (0)

    buf_init(&rawm);
    raw_merge(&in, d, &rawm);
    for (i = 0; i < in.nraw; i++) ST_ADD(in.raw[i].t, K_RAW, 0, 0, 0, i);
    if (in.ndac) {
        have_dac = 1;
        dac_streams(&in, &dac_bank, &runs, &nr);
        in.out_ym_clock = in.ym_clock;
        for (i = 0; i < nr; i++) ST_ADD(runs[i].t, K_DAC, 0, 0, 0, i);
    }
    for (i = 0; i < in.nev; i++) {
        const YEv *e = &in.ev[i];
        lfo_advance(e->t);
        sd_advance(e->t);
        S->t = e->t;
        if (!e->psg) {
            if (have_dac && ((e->port == 0 && e->reg == 0x2B) || (e->port == 1 && e->reg == 0xB6))) {
                int k = (e->port == 0) ? 0 : 1;
                if (last_raw[k] != e->val) {
                    last_raw[k] = e->val;
                    ST_ADD(e->t, K_YMRAW, e->port, e->reg, e->val, 0);
                }
            }
            ym_write(e->port, e->reg, e->val);
        } else {
            ST_ADD(e->t, K_PSG, e->val, 0, 0, 0);
        }
    }
    end_t = in.end_t > (long)in.total ? in.end_t : (long)in.total;
    lfo_advance(end_t);
    sd_advance(end_t);
    for (i = 0; i < S->nev; i++) {
        const OplW *e = &S->ev[i];
        if (nst >= capst) { capst = capst ? capst * 2 : 16384; st = (SItem *)xrealloc(st, sizeof(SItem) * (size_t)capst); }
        st[nst].t = e->t; st[nst].seq = e->seq; st[nst].kind = K_OPL;
        st[nst].a = e->port; st[nst].b = e->reg; st[nst].c = e->val; st[nst].run = 0;
        nst++;
    }
    qsort(st, (size_t)nst, sizeof(SItem), cmp_sitem);

    if (have_dac) {
        long r;
        v3(&v, 0x52, 0x2B, 0x80);
        buf_byte(&v, 0x67); buf_byte(&v, 0x66); buf_byte(&v, 0x00);
        buf_u32(&v, (unsigned long)dac_bank.n);
        buf_add(&v, dac_bank.p, dac_bank.n);
        { static const unsigned char a[5] = { 0x90, 0x00, 0x02, 0x00, 0x2A }; buf_add(&v, a, 5); }
        { static const unsigned char a[5] = { 0x91, 0x00, 0x00, 0x01, 0x00 }; buf_add(&v, a, 5); }
        for (r = 0; r < 2; r++) {
            if (vdac_block(&dac_bank, runs, nr, VDAC_RATES[r], &blk)) {
                buf_byte(&v, 0x67); buf_byte(&v, 0x66); buf_byte(&v, 0x3F);
                buf_u32(&v, (unsigned long)blk.n);
                buf_add(&v, blk.p, blk.n);
            }
        }
    }

#define DATA_N (v.n)
    {
        long base = 0;
        (void)base;
    }
    for (i = 0; i < nst; i++) {
        const SItem *s = &st[i];
        if (in.loop_t >= 0 && loop_pos < 0 && s->t >= in.loop_t) {
            if (in.loop_t > cur) { vwait(&v, in.loop_t - cur); cur = in.loop_t; }
            loop_pos = v.n;
        }
        if (s->t > cur) { vwait(&v, s->t - cur); cur = s->t; }
        if (s->kind == K_OPL) v3(&v, s->a == 1 ? 0x5F : 0x5E, s->b, s->c);
        else if (s->kind == K_PSG) { buf_byte(&v, 0x50); buf_byte(&v, s->a); }
        else if (s->kind == K_YMRAW) v3(&v, s->a == 1 ? 0x53 : 0x52, s->b, s->c);
        else if (s->kind == K_RAW) buf_add(&v, in.raw[s->run].off < 0 ? rawm.p + (-in.raw[s->run].off - 1) : d->p + in.raw[s->run].off, in.raw[s->run].len);
        else {
            const Run *r = &runs[s->run];
            long ihz = (long)pyround(r->hz);
            if (ihz != last_hz) {
                buf_byte(&v, 0x92); buf_byte(&v, 0); buf_u32(&v, (unsigned long)ihz);
                last_hz = ihz;
            }
            buf_byte(&v, 0x93); buf_byte(&v, 0); buf_u32(&v, (unsigned long)r->off);
            buf_byte(&v, 1); buf_u32(&v, (unsigned long)r->cnt);
        }
    }
    if (in.loop_t >= 0 && loop_pos < 0) {
        if (in.loop_t > cur) { vwait(&v, in.loop_t - cur); cur = in.loop_t; }
        loop_pos = v.n;
    }
    if (end_t > cur) { vwait(&v, end_t - cur); cur = end_t; }

    {
        long loop_samples = in.loop_t >= 0 ? cur - in.loop_t : 0;
        int nvol = 0, vch[2], vv[2];
        res->n = 0;
        buf_reserve(res, 0x100 + v.n + 1 + (in.gd3_off >= 0 ? in.gd3_len : 0));
        memset(res->p, 0, 0x100);
        res->n = 0x100;
        memcpy(res->p, "Vgm ", 4);
        wr32(res, 0x08, 0x171);
        wr32(res, 0x0C, in.sn_clock);
        wr32(res, 0x18, (unsigned long)cur);
        for (k = 0; k < in.n_snflags; k++) res->p[0x28 + k] = in.sn_flags[k];
        wr32(res, 0x34, 0x100 - 0x34);
        wr32(res, 0x5C, OPLCLK);
        if (in.nraw && in.version >= 0x151) {
            wr32(res, 0x40, rd32(d, 0x40));
            wr32(res, 0x6C, rd32(d, 0x6C));
        }
        if (in.has_ay && in.version >= 0x151) {
            wr32(res, 0x74, rd32(d, 0x74));
            wr32(res, 0x78, rd32(d, 0x78));
        }
        if (in.sn_clock && PSG_VGMPLAY_VOL != 0x100) { vch[nvol] = 0; vv[nvol] = PSG_VGMPLAY_VOL; nvol++; }
        if (in.out_ym_clock) {
            wr32(res, 0x2C, in.out_ym_clock);
            vch[nvol] = 2; vv[nvol] = DAC_VGMPLAY_VOL; nvol++;
        }
        if (nvol) {
            wr32(res, 0xBC, 0xE0 - 0xBC);
            wr32(res, 0xE0, 0x0C); wr32(res, 0xE4, 0); wr32(res, 0xE8, 4);
            res->p[0xEC] = (unsigned char)nvol;
            for (k = 0; k < nvol; k++) {
                unsigned vol = 0x8000 | vv[k];
                res->p[0xED + 4 * k] = (unsigned char)vch[k];
                res->p[0xEE + 4 * k] = 0;
                res->p[0xEF + 4 * k] = (unsigned char)(vol & 0xFF);
                res->p[0xF0 + 4 * k] = (unsigned char)(vol >> 8);
            }
        }
        if (loop_pos >= 0 && loop_samples) {
            wr32(res, 0x1C, (unsigned long)(0x100 + loop_pos - 0x1C));
            wr32(res, 0x20, (unsigned long)loop_samples);
        }
        buf_add(res, v.p, v.n);
        buf_byte(res, 0x66);
        if (in.gd3_off >= 0) {
            wr32(res, 0x14, (unsigned long)(res->n - 0x14));
            buf_add(res, d->p + in.gd3_off, in.gd3_len);
        }
        wr32(res, 0x04, (unsigned long)(res->n - 4));
        {
            char b1[64] = "", b2[64] = "";
            if (in.loop_t >= 0) sprintf(b1, g_en ? ", loop from %.1f s" : ", bucle desde %.1f s", in.loop_t / 44100.0);
            if (have_dac) sprintf(b2, g_en ? ", DAC: %ld samples in %ld KB" : ", DAC: %ld muestras en %ld KB", nr, (dac_bank.n + 1023) / 1024);
            sprintf(msg, "%.1f s%s%s%s", cur / 44100.0, b1, b2,
                    g_md_ssgeg ? (g_en ? ", uses SSG-EG" : ", usa SSG-EG") : "");
        }
    }
    free(S->ev); free(S); S = NULL;
    free(st); free(runs);
    free(in.ev); free(in.dacw); free(in.raw); buf_free(&in.bank); buf_free(&rawm);
    buf_free(&v); buf_free(&dac_bank); buf_free(&blk);
    return 1;
}
