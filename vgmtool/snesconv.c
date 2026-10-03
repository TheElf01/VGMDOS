#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "vgmlib.h"
#include "spc/spcemu.h"

extern int g_en;

#define RF_CLOCK   12500001UL
#define RF_RATE    (12500000.0 / 384.0)
#define RING       0x2000L
#define RING_DATA  (RING - 1)
#define CHUNK      512L
#define NCH        8
#define STEP       80
#define LOOP_MIN   1024L

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

typedef struct {
    int start, loopa;
    unsigned long hash;
    long off, len, loop;
    double peak;
    int dec;
    int up;
} Smp;
static Smp *g_smp; static int g_nsmp, g_capsmp;
static Buf g_bank;

static void brr_block(const unsigned char *b, int *p1, int *p2, int *out)
{
    int header = b[0], shift = header >> 4, filter = header & 0x0C, i;
    for (i = 0; i < 16; i++) {
        int nib = (i & 1) ? (b[1 + i / 2] & 0x0F) : (b[1 + i / 2] >> 4);
        int s, a1 = *p1, a2 = *p2 >> 1;
        if (nib & 8) nib -= 16;
        s = (nib << shift) >> 1;
        if (shift >= 0xD) s = s < 0 ? -2048 : 0;
        if (filter >= 8) {
            s += a1; s -= a2;
            if (filter == 8) { s += a2 >> 4; s += (a1 * -3) >> 6; }
            else { s += (a1 * -13) >> 7; s += (a2 * 3) >> 4; }
        } else if (filter) { s += a1 >> 1; s += (-a1) >> 5; }
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        s = (short)(s * 2);
        *p2 = *p1; *p1 = s;
        out[i] = s;
    }
}

static int get_noise(void)
{
    int i;
    long k;
    unsigned lfsr = 0x4000;
    Smp *s;
    for (i = 0; i < g_nsmp; i++) if (g_smp[i].start == -1) return i;
    if (g_nsmp >= g_capsmp) {
        g_capsmp = g_capsmp ? g_capsmp * 2 : 64;
        g_smp = (Smp *)xrealloc(g_smp, sizeof(Smp) * (size_t)g_capsmp);
    }
    s = &g_smp[g_nsmp];
    s->start = -1; s->loopa = 0; s->hash = 0;
    s->off = g_bank.n; s->len = 8192; s->loop = 0; s->peak = 32767; s->dec = 1; s->up = 1;
    for (k = 0; k < 8192; k++) {
        unsigned fb = (lfsr << 13 ^ lfsr << 14) & 0x4000;
        lfsr = fb ^ (lfsr >> 1);
        buf_byte(&g_bank, sm_byte((lfsr & 0x4000) ? 100 : -100));
    }
    for (k = 0; k < 64; k++) buf_byte(&g_bank, 0);
    return g_nsmp++;
}

static unsigned long smp_hash(const unsigned char *ram, int start)
{
    unsigned long h = 5381;
    int a = start, nb, j;
    for (nb = 0; nb < 8000; nb++) {
        for (j = 0; j < 9; j++) h = h * 33 + ram[(a + j) & 0xFFFF];
        if (ram[a & 0xFFFF] & 1) break;
        a = (a + 9) & 0xFFFF;
    }
    return h;
}

typedef struct { int start, loopa; unsigned long hash; int minp, maxp; } Use;
static Use *g_use; static int g_nuse;

static void use_note(const unsigned char *ram, int start, int loopa, int pitch)
{
    unsigned long h = smp_hash(ram, start);
    int i;
    for (i = 0; i < g_nuse; i++)
        if (g_use[i].start == start && g_use[i].loopa == loopa && g_use[i].hash == h) {
            if (pitch < g_use[i].minp) g_use[i].minp = pitch;
            if (pitch > g_use[i].maxp) g_use[i].maxp = pitch;
            return;
        }
    g_use = (Use *)xrealloc(g_use, sizeof(Use) * (size_t)(g_nuse + 1));
    g_use[g_nuse].start = start; g_use[g_nuse].loopa = loopa; g_use[g_nuse].hash = h; g_use[g_nuse].minp = pitch; g_use[g_nuse].maxp = pitch;
    g_nuse++;
}

static int use_dec(int start, int loopa, unsigned long h)
{
    int i;
    for (i = 0; i < g_nuse; i++)
        if (g_use[i].start == start && g_use[i].loopa == loopa && g_use[i].hash == h) {
            double r = g_use[i].minp * 32000.0 / 4096.0;
            if (r >= 3 * 18000.0) return 3;
            if (r >= 2 * 18000.0) return 2;
            return 1;
        }
    return 1;
}

static int get_smp(const unsigned char *ram, int start, int loopa, int pitch)
{
    int i, p1 = 0, p2 = 0, loops = 0, blk[16], dec, up;
    long n = 0, cap = 4096, k, loopi = -1, l1 = 0, m;
    unsigned long h;
    int a;
    double *x, peak = 0;
    int *raw;
    Smp *s;
    h = smp_hash(ram, start);
    dec = use_dec(start, loopa, h);
    up = 1;
    if (dec == 1 && pitch > 0) {
        double r = pitch * 32000.0 / 4096.0;
        while (up < 8 && r * up < 11000.0 && r * up * 2 <= 40000.0) up *= 2;
    }
    for (i = 0; i < g_nsmp; i++)
        if (g_smp[i].start == start && g_smp[i].loopa == loopa && g_smp[i].hash == h && g_smp[i].up == up) return i;
    raw = (int *)xmalloc(sizeof(int) * (size_t)cap);
    a = start;
    for (;;) {
        int hd = ram[a & 0xFFFF];
        unsigned char b[9];
        int j;
        for (j = 0; j < 9; j++) b[j] = ram[(a + j) & 0xFFFF];
        brr_block(b, &p1, &p2, blk);
        if (n + 16 > cap) { cap *= 2; raw = (int *)xrealloc(raw, sizeof(int) * (size_t)cap); }
        for (j = 0; j < 16; j++) raw[n++] = blk[j];
        if (n > 300000L) break;
        if (hd & 1) {
            if (!(hd & 2)) break;
            if (loops == 0) { l1 = n; loopi = n; loops = 1; a = loopa; continue; }
            break;
        }
        a = (a + 9) & 0xFFFF;
    }
    if (loopi >= 0) {
        long ll = n - loopi, rep;
        if (ll <= 0) loopi = -1;
        else {
            rep = (LOOP_MIN + ll - 1) / ll;
            dec = use_dec(start, loopa, h);
            while ((ll * rep) % dec) rep++;
            if (rep > 1) {
                raw = (int *)xrealloc(raw, sizeof(int) * (size_t)(n + ll * (rep - 1) + 16));
                for (k = 1; k < rep; k++) { memcpy(raw + n, raw + loopi, sizeof(int) * (size_t)ll); n += ll; }
            }
        }
    }
    (void)l1;
    x = (double *)xmalloc(sizeof(double) * (size_t)(n + 1));
    for (k = 0; k < n; k++) {
        double pv = raw[k > 0 ? k - 1 : 0], nx = raw[k + 1 < n ? k + 1 : (loopi >= 0 ? loopi : k)];
        x[k] = 0.182 * pv + 0.636 * raw[k] + 0.182 * nx;
        if (fabs(x[k]) > peak) peak = fabs(x[k]);
    }
    if (dec > 1) {
        long j0 = loopi >= 0 ? (loopi + dec - 1) / dec : 0, per = loopi >= 0 ? n - loopi : 0;
        double *y, hh[31], sum = 0;
        int tp;
        m = loopi >= 0 ? j0 + per / dec : n / dec;
        for (tp = -15; tp <= 15; tp++) {
            double xx = tp * 0.9 / dec, w = 0.5 + 0.5 * cos(3.14159265 * tp / 16.0);
            hh[tp + 15] = (tp ? sin(3.14159265 * xx) / (3.14159265 * xx) : 1.0) * w;
            sum += hh[tp + 15];
        }
        y = (double *)xmalloc(sizeof(double) * (size_t)(m + 1));
        peak = 0;
        for (k = 0; k < m; k++) {
            long c0 = k * dec;
            double acc = 0;
            for (tp = -15; tp <= 15; tp++) {
                long q = c0 + tp;
                if (q < 0) q = 0;
                if (q >= n) q = per > 0 ? loopi + (q - loopi) % per : n - 1;
                acc += hh[tp + 15] * x[q];
            }
            y[k] = acc / sum;
            if (fabs(y[k]) > peak) peak = fabs(y[k]);
        }
        free(x); x = y; n = m;
        if (loopi >= 0) loopi = j0;
    }
    if (up > 1) {
        long per = loopi >= 0 ? n - loopi : 0, j;
        double *y;
        int tp;
        m = n * up;
        y = (double *)xmalloc(sizeof(double) * (size_t)(m + 1));
        peak = 0;
        for (j = 0; j < m; j++) {
            long q0 = j / up;
            double fr = (double)(j % up) / up, acc = 0, ws = 0;
            for (tp = -7; tp <= 8; tp++) {
                long q = q0 + tp;
                double xx = tp - fr, w, s;
                w = 0.5 + 0.5 * cos(3.14159265 * xx / 8.5);
                s = (xx > -1e-9 && xx < 1e-9) ? 1.0 : sin(3.14159265 * xx) / (3.14159265 * xx);
                if (q < 0) q = 0;
                if (q >= n) q = per > 0 ? loopi + (q - loopi) % per : n - 1;
                acc += x[q] * s * w; ws += s * w;
            }
            y[j] = acc / ws;
            if (fabs(y[j]) > peak) peak = fabs(y[j]);
        }
        free(x); x = y; n = m;
        if (loopi >= 0) loopi *= up;
    }
    if (g_nsmp >= g_capsmp) {
        g_capsmp = g_capsmp ? g_capsmp * 2 : 64;
        g_smp = (Smp *)xrealloc(g_smp, sizeof(Smp) * (size_t)g_capsmp);
    }
    s = &g_smp[g_nsmp];
    s->start = start; s->loopa = loopa; s->hash = h; s->dec = dec; s->up = up;
    s->off = g_bank.n; s->len = n; s->loop = loopi; s->peak = peak;
    for (k = 0; k < n; k++) {
        int q = peak > 0 ? (int)floor(x[k] * 127.0 / peak + 0.5) : 0;
        buf_byte(&g_bank, sm_byte(q));
    }
    for (k = 0; k < 64; k++) buf_byte(&g_bank, 0);
    free(raw); free(x);
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
    long kon;
    long tenv, tfd;
    long tzero;
    int quiet;
    long base;
} Ch;
static Ch g_ch[NCH];
static long g_base = 0;
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
    return s->loop >= 0 ? 0x7FFFFFFFL : s->len + 64 - c->base;
}

static void ch_write(int k, long t, long b, long n)
{
    Ch *c = &g_ch[k];
    const Smp *s = &g_smp[c->smp];
    while (n > 0) {
        long si, rp, m, lim;
        long bb = b + c->base;
        if (s->loop >= 0) {
            si = bb < s->len ? bb : s->loop + (bb - s->loop) % (s->len - s->loop);
            lim = s->len - si;
        } else {
            if (bb >= s->len + 64) return;
            si = bb; lim = s->len + 64 - si;
        }
        rp = b % RING_DATA;
        m = n;
        if (m > RING_DATA - rp) m = RING_DATA - rp;
        if (m > lim) m = lim;
        rf_copy(t, k * 2, s->off + si, rp, m);
        b += m; n -= m;
    }
}

static void ch_settend(Ch *c)
{
    const Smp *s = &g_smp[c->smp];
    if (s->loop >= 0) c->tend = 0x7FFFFFFFL;
    else c->tend = c->t0 + (long)ceil(((double)(s->len - c->base) - c->pos0) * 44100.0 / c->rate) + 1;
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
    c->on = 1; c->smp = smp; c->base = g_base;
    c->fd = rate_fd(rate);
    c->rate = c->fd * RF_RATE / 2048.0;
    c->pos0 = 0; c->t0 = t; c->written = 0;
    c->env = env; c->pan = pan; c->tenv = c->tfd = t;
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

static void ch_stop(int k, long t)
{
    if (!g_ch[k].on) return;
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

static long txt_num(const unsigned char *p, int n)
{
    long v = 0;
    int i, any = 0;
    for (i = 0; i < n && p[i]; i++) {
        if (p[i] < '0' || p[i] > '9') return -1;
        v = v * 10 + (p[i] - '0'); any = 1;
    }
    return any ? v : -1;
}

static void gd3_str(Buf *g, const unsigned char *p, int n)
{
    int i;
    for (i = 0; i < n && p && p[i]; i++) { buf_byte(g, p[i]); buf_byte(g, 0); }
    buf_byte(g, 0); buf_byte(g, 0);
}

#define GAIN 1.8

int snes_convertir(const Buf *d, Buf *res, char *msg)
{
    void *e;
    long secs, fade_ms, n32, t32, i, t = 0;
    int k;
    Buf syn, gd3;
    const unsigned char *tag = d->p;
    msg[0] = 0;
    if (d->n < 0x10200 || memcmp(d->p, "SNES-SPC700 Sound File Data", 27) != 0) return 0;
    e = spce_open(d->p, d->n);
    if (!e) return -1;
    secs = -1; fade_ms = -1;
    if (d->p[0x23] == 26) {
        secs = txt_num(tag + 0xA9, 3);
        fade_ms = txt_num(tag + 0xAC, 5);
        if (secs < 0) {
            secs = tag[0xA9] | (tag[0xAA] << 8);
            fade_ms = (long)(tag[0xAC] | (tag[0xAD] << 8) | ((long)tag[0xAE] << 16));
        }
    }
    if (secs <= 0 || secs > 1200) secs = 180;
    if (fade_ms < 0 || fade_ms > 60000L) fade_ms = 10000;
    n32 = (secs * 1000L + fade_ms) * 32;
    buf_init(&syn); buf_init(&gd3); buf_init(&g_bank); buf_init(&g_evdata);
    g_smp = NULL; g_nsmp = g_capsmp = 0; g_ev = NULL; g_nev = g_capev = g_seq = 0;
    memset(g_ch, 0, sizeof(g_ch));
    g_mask = 0xFF;
    rf_reg(0, 7, 0x80);
    rf_reg(0, 8, 0xFF);
    for (k = 0; k < NCH; k++) {
        unsigned char x[4];
        rf_reg(0, 7, 0x80 | (k * 2 + 1));
        x[0] = 0xC2; x[1] = 0xFF; x[2] = 0x0F; x[3] = 0xFF;
        emit(0, x, 4);
    }
    g_use = NULL; g_nuse = 0;
    {
        long kon[NCH];
        for (k = 0; k < NCH; k++) kon[k] = 0;
        for (t32 = 0; t32 < n32; t32 += STEP) {
            spce_run(e, STEP, NULL);
            for (k = 0; k < NCH; k++) {
                SpcVoice v;
                spce_voice(e, k, &v);
                if (v.kon != kon[k]) {
                    const unsigned char *ram = spce_ram(e);
                    kon[k] = v.kon;
                    if (!v.non)
                        use_note(ram, v.start, ram[(v.dira + 2) & 0xFFFF] | (ram[(v.dira + 3) & 0xFFFF] << 8), v.pitch);
                }
                if (v.env > 0 && !v.non && g_nuse && v.pitch > 0) {
                    int i;
                    for (i = g_nuse - 1; i >= 0; i--) if (g_use[i].start == v.start) { if (v.pitch < g_use[i].minp) g_use[i].minp = v.pitch; if (v.pitch > g_use[i].maxp) g_use[i].maxp = v.pitch; break; }
                }
            }
        }
        spce_close(e);
        e = spce_open(d->p, d->n);
        if (!e) { free(g_use); return -1; }
    }
    for (t32 = 0; t32 < n32; t32 += STEP) {
        double fade = 1.0;
        int mvol;
        spce_run(e, STEP, NULL);
        t = (t32 + STEP) / 320L * 441L + (t32 + STEP) % 320L * 441L / 320L;
        flush_until(t);
        if (t32 > secs * 32000L) fade = 1.0 - (double)(t32 - secs * 32000L) / (fade_ms * 32.0);
        if (fade < 0) fade = 0;
        mvol = spce_reg(e, 0x0C);
        if ((signed char)spce_reg(e, 0x1C) > (signed char)mvol) mvol = spce_reg(e, 0x1C);
        mvol = abs((signed char)mvol);
        for (k = 0; k < NCH; k++) {
            SpcVoice v;
            Ch *c = &g_ch[k];
            double al, ar, a, pk;
            int env, pan, nl, nr;
            unsigned fd;
            spce_voice(e, k, &v);
            if (v.kon != c->kon) {
                const unsigned char *ram = spce_ram(e);
                int sm, loopa = ram[(v.dira + 2) & 0xFFFF] | (ram[(v.dira + 3) & 0xFFFF] << 8);
                c->kon = v.kon;
                sm = v.non ? get_noise() : get_smp(ram, v.start, loopa, v.pitch);
                ch_start(k, t, sm, 1000.0, 0, 0);
                c->env = -1; c->tzero = -1; c->quiet = 0;
            }
            if (!c->on && !c->quiet) continue;
            pk = g_smp[c->smp].peak;
            al = fabs((double)v.voll) / 128.0; ar = fabs((double)v.volr) / 128.0;
            a = al > ar ? al : ar;
            env = (int)floor(255.0 * GAIN * fade * (v.env / 2048.0) * a * (mvol / 128.0) * pk / 32768.0 + 0.5);
            if (env > 255) env = 255;
            if (v.release && v.env == 0) { ch_stop(k, t); c->quiet = 0; continue; }
            if (c->quiet) {
                const Smp *s = &g_smp[c->smp];
                if (env == 0 || s->loop < 0) continue;
                c->quiet = 0;
                g_base = s->loop;
                ch_start(k, t, c->smp, c->rate, 0, 0);
                g_base = 0;
                c->env = -1;
            }
            if (env == 0 && c->env == 0) {
                if (c->tzero < 0) c->tzero = t;
                else if (t - c->tzero >= 441) { ch_stop(k, t); c->quiet = 1; c->tzero = -1; continue; }
            } else c->tzero = -1;
            nl = a > 0 ? (int)floor(15.0 * al / a + 0.5) : 0;
            nr = a > 0 ? (int)floor(15.0 * ar / a + 0.5) : 0;
            pan = nl | (nr << 4);
            if (v.non) {
                static const int nrate[32] = { 0, 16, 21, 25, 31, 42, 50, 63, 83, 100, 125, 167, 200, 250, 333,
                    400, 500, 667, 800, 1000, 1300, 1600, 2000, 2700, 3200, 4000, 5300, 6400, 8000, 10700, 16000, 32000 };
                fd = rate_fd((double)nrate[spce_reg(e, 0x6C) & 31] * 2.0);
            } else
                fd = rate_fd((double)v.pitch * 32000.0 / 4096.0 / g_smp[c->smp].dec * g_smp[c->smp].up);
            if (fd != c->fd) {
                double r = (double)fd / c->fd;
                if (c->env < 0 || r > 1.003 || r < 0.997 || t - c->tfd >= 441) ch_setfd(k, t, fd);
            }
            if (env != c->env || pan != c->pan) {
                int d0 = abs(env - c->env), lim = c->env / 8;
                if (lim < 2) lim = 2;
                if (c->env < 0 || pan != c->pan || d0 >= lim || env == 0 || t - c->tenv >= 882) {
                    rf_reg(t, 7, 0xC0 | k);
                    if (env != c->env) rf_reg(t, 0, env);
                    if (pan != c->pan) rf_reg(t, 1, pan);
                    c->env = env; c->pan = pan; c->tenv = t;
                }
            }
        }
    }
    flush_until(t);
    for (k = 0; k < NCH; k++) ch_stop(k, t);
    spce_close(e);
    free(g_use); g_use = NULL; g_nuse = 0;
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
    buf_reserve(&syn, 0x100);
    memset(syn.p, 0, 0x100); syn.n = 0x100;
    memcpy(syn.p, "Vgm ", 4);
    wr32(&syn, 0x08, 0x171);
    wr32(&syn, 0x18, (unsigned long)t);
    wr32(&syn, 0x34, 0x100 - 0x34);
    wr32(&syn, 0x6C, RF_CLOCK);
    buf_byte(&syn, 0x67); buf_byte(&syn, 0x66); buf_byte(&syn, 0x02);
    buf_u32(&syn, (unsigned long)g_bank.n);
    buf_add(&syn, g_bank.p, g_bank.n);
    {
        long cur = 0;
        for (i = 0; i < g_nev; i++) {
            long w = g_ev[i].t - cur;
            while (w > 0) { long q = w > 65535 ? 65535 : w; buf_byte(&syn, 0x61); buf_u16(&syn, (unsigned)q); w -= q; }
            if (g_ev[i].t > cur) cur = g_ev[i].t;
            buf_add(&syn, g_evdata.p + g_ev[i].off, g_ev[i].len);
        }
        if (t > cur) { long w = t - cur; while (w > 0) { long q = w > 65535 ? 65535 : w; buf_byte(&syn, 0x61); buf_u16(&syn, (unsigned)q); w -= q; } }
    }
    buf_byte(&syn, 0x66);
    {
        const unsigned char *song = d->p + 0x2E, *game = d->p + 0x4E, *art = d->p + 0xB1;
        Buf b;
        buf_init(&b);
        if (d->p[0x23] != 26) song = game = art = NULL;
        gd3_str(&b, song, 32); gd3_str(&b, NULL, 0);
        gd3_str(&b, game, 32); gd3_str(&b, NULL, 0);
        gd3_str(&b, (const unsigned char *)"SNES", 4); gd3_str(&b, NULL, 0);
        gd3_str(&b, art, 32); gd3_str(&b, NULL, 0);
        gd3_str(&b, NULL, 0);
        gd3_str(&b, (const unsigned char *)"vgmtool", 7);
        gd3_str(&b, NULL, 0);
        wr32(&syn, 0x14, (unsigned long)(syn.n - 0x14));
        buf_add(&syn, "Gd3 ", 4); buf_u32(&syn, 0x100); buf_u32(&syn, (unsigned long)b.n);
        buf_add(&syn, b.p, b.n);
        buf_free(&b);
    }
    wr32(&syn, 0x04, (unsigned long)(syn.n - 4));
    res->n = 0;
    buf_add(res, syn.p, syn.n);
    sprintf(msg, "SNES, PCM %ld KB, %d muestras", (g_bank.n + 1023) / 1024, g_nsmp);
    buf_free(&syn); buf_free(&gd3); buf_free(&g_bank); buf_free(&g_evdata);
    free(g_smp); free(g_ev);
    return 1;
}
