#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "vgmlib.h"

#include "pred.h"

#define BANK_MAX 60000L
#define MIN_DDA 200
static const int SILENCIOS[6] = { 48, 24, 12, 6, 3, 2 };

#define NES_RING 32768L
#define NES_TROZO 4096L
#define NES_SIL 12

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

enum { EV_W, EV_P, EV_N, EV_X };
typedef struct {
    unsigned char tipo, a, v;
    long w;
    long off, len;
} Ev;

typedef struct { Ev *e; long n, cap; } Evs;

static Ev *ev_nuevo(Evs *l)
{
    if (l->n >= l->cap) {
        l->cap = l->cap ? l->cap * 2 : 4096;
        l->e = (Ev *)xrealloc(l->e, sizeof(Ev) * (size_t)l->cap);
    }
    memset(&l->e[l->n], 0, sizeof(Ev));
    return &l->e[l->n++];
}

static int parsear(const Buf *d, Evs *ev, long *loop_idx)
{
    long off = (rd32(d, 8) >= 0x150 && rd32(d, 0x34)) ? 0x34 + (long)rd32(d, 0x34) : 0x40;
    long loop = rd32(d, 0x1C) ? (long)rd32(d, 0x1C) + 0x1C : -1;
    long i = off, n = d->n;
    ev->n = 0;
    *loop_idx = -1;
    while (i < n) {
        int c = d->p[i];
        Ev *e;
        if (i == loop) *loop_idx = ev->n;
        if (c == 0x66) break;
        if (c == 0xB9 || c == 0xB4) {
            if (i + 2 >= n) return 1;
            e = ev_nuevo(ev);
            e->tipo = (c == 0xB9) ? EV_P : EV_N;
            e->a = d->p[i + 1]; e->v = d->p[i + 2];
            i += 3;
        } else if (c == 0x61) {
            if (i + 2 >= n) return 1;
            e = ev_nuevo(ev); e->tipo = EV_W;
            e->w = d->p[i + 1] | (d->p[i + 2] << 8);
            i += 3;
        } else if (c == 0x62) { e = ev_nuevo(ev); e->tipo = EV_W; e->w = 735; i++; }
        else if (c == 0x63) { e = ev_nuevo(ev); e->tipo = EV_W; e->w = 882; i++; }
        else if (c >= 0x70 && c <= 0x7F) { e = ev_nuevo(ev); e->tipo = EV_W; e->w = (c & 15) + 1; i++; }
        else if (c == 0x67) {
            long sz, L;
            if (i + 7 > n) return 1;
            sz = (long)(rd32(d, i + 3) & 0x7FFFFFFFUL);
            L = 7 + sz;
            e = ev_nuevo(ev); e->tipo = EV_X; e->off = i;
            e->len = (i + L > n) ? n - i : L;
            i += L;
        } else {
            long L;
            if (c == 0x4F || c == 0x50 || (c >= 0x30 && c <= 0x3F)) L = 2;
            else if ((c >= 0x40 && c <= 0x4E) || (c >= 0x51 && c <= 0x5F) || (c >= 0xA0 && c <= 0xBF)) L = 3;
            else if (c >= 0xC0 && c <= 0xDF) L = 4;
            else if (c >= 0xE0) L = 5;
            else if (c >= 0x80 && c <= 0x8F) L = 1;
            else if (c == 0x90 || c == 0x91 || c == 0x95) L = 5;
            else if (c == 0x92) L = 6;
            else if (c == 0x93) L = 11;
            else if (c == 0x94) L = 2;
            else L = 1;
            e = ev_nuevo(ev); e->tipo = EV_X; e->off = i;
            e->len = (i + L > n) ? n - i : L;
            i += L;
        }
    }
    return 0;
}

typedef struct { long k, t; unsigned char v; } Wr;
typedef struct { Wr *w; long n, cap; } Wrs;

static void wr_add(Wrs *l, long k, long t, int v)
{
    if (l->n >= l->cap) {
        l->cap = l->cap ? l->cap * 2 : 1024;
        l->w = (Wr *)xrealloc(l->w, sizeof(Wr) * (size_t)l->cap);
    }
    l->w[l->n].k = k; l->w[l->n].t = t; l->w[l->n].v = (unsigned char)v;
    l->n++;
}

static void escrituras_dda(const Evs *ev, Wrs w[6])
{
    long t = 0, k;
    int ch = 0, on[6] = {0,0,0,0,0,0}, dm[6] = {0,0,0,0,0,0};
    for (k = 0; k < ev->n; k++) {
        const Ev *e = &ev->e[k];
        if (e->tipo == EV_W) t += e->w;
        else if (e->tipo == EV_P) {
            int a = e->a, v = e->v;
            if (a == 0) ch = v & 7;
            else if (ch < 6) {
                if (a == 4) { on[ch] = v >> 7; dm[ch] = (v >> 6) & 1; }
                else if (a == 6 && on[ch] && dm[ch]) wr_add(&w[ch], k, t, v & 31);
            }
        }
    }
}

static void escrituras_nes(const Evs *ev, Wrs *w)
{
    long t = 0, k;
    for (k = 0; k < ev->n; k++) {
        const Ev *e = &ev->e[k];
        if (e->tipo == EV_W) t += e->w;
        else if (e->tipo == EV_N && e->a == 0x11) wr_add(w, k, t, e->v & 0x7F);
    }
}

static int cmp_long(const void *a, const void *b)
{
    long x = *(const long *)a, y = *(const long *)b;
    return (x > y) - (x < y);
}

static long intervalos(const Wrs *l, long **iv)
{
    long k, n = l->n > 1 ? l->n - 1 : 0;
    *iv = (long *)xmalloc(sizeof(long) * (size_t)(n + 1));
    for (k = 0; k < n; k++) (*iv)[k] = l->w[k + 1].t - l->w[k].t;
    qsort(*iv, (size_t)n, sizeof(long), cmp_long);
    return n;
}

static long mediana_min1(const Wrs *l)
{
    long *iv, n = intervalos(l, &iv), med;
    med = n ? iv[n / 2] : 6;
    free(iv);
    return med < 1 ? 1 : med;
}

static long redondear(double x)
{
    double f = (double)(long)x;
    double r = x - f;
    if (r > 0.5) return (long)f + 1;
    if (r < 0.5) return (long)f;
    return ((long)f & 1) ? (long)f + 1 : (long)f;
}

static long frecuencia(const Wrs *l)
{
    long *iv, n = intervalos(l, &iv), med, k, cnt = 0;
    double suma = 0;
    if (!n) { free(iv); return 7000; }
    med = iv[n / 2];
    for (k = 0; k < n; k++) if (iv[k] <= 3 * med) { suma += iv[k]; cnt++; }
    free(iv);
    if (cnt == 0 || suma == 0) return -1;
    return redondear(44100.0 / (suma / (double)cnt));
}

typedef struct { long k, off, len; int uid; } Tz;
typedef struct { Tz *t; long n, cap; Buf pool; } Tzs;

static void tz_add(Tzs *l, long k, const unsigned char *b, long len)
{
    if (l->n >= l->cap) {
        l->cap = l->cap ? l->cap * 2 : 256;
        l->t = (Tz *)xrealloc(l->t, sizeof(Tz) * (size_t)l->cap);
    }
    l->t[l->n].k = k; l->t[l->n].off = l->pool.n; l->t[l->n].len = len; l->t[l->n].uid = -1;
    buf_add(&l->pool, b, len);
    l->n++;
}

static void trozos(const Wrs *lista, int sil, Tzs *res)
{
    long med, ult = 0, ini = 0, j;
    int hay_ult = 0;
    long run = 0;
    Buf cur;
    if (!lista->n) return;
    med = mediana_min1(lista);
    buf_init(&cur);
    for (j = 0; j < lista->n; j++) {
        long k = lista->w[j].k, t = lista->w[j].t;
        int v = lista->w[j].v;
        if (hay_ult && t - ult > 8 * med && cur.n) {
            tz_add(res, ini, cur.p, cur.n);
            cur.n = 0;
        }
        if (!cur.n) { ini = k; run = 0; }
        run = (cur.n && cur.p[cur.n - 1] == v) ? run + 1 : 1;
        buf_byte(&cur, v);
        ult = t; hay_ult = 1;
        if (cur.n > sil && run >= sil) {
            tz_add(res, ini, cur.p, cur.n - sil + 1);
            cur.n = 0;
        }
    }
    if (cur.n) tz_add(res, ini, cur.p, cur.n);
    buf_free(&cur);
}

static const Tzs *g_tz;
static int cmp_tz(const void *a, const void *b)
{
    const Tz *x = &g_tz->t[*(const long *)a], *y = &g_tz->t[*(const long *)b];
    int r;
    if (x->len != y->len) return (x->len > y->len) ? -1 : 1;
    r = memcmp(g_tz->pool.p + x->off, g_tz->pool.p + y->off, (size_t)x->len);
    return r;
}

static int armar_banco(Tzs *todos, Buf *banco, long *pos)
{
    long *idx = (long *)xmalloc(sizeof(long) * (size_t)(todos->n + 1));
    long k, o = 0;
    for (k = 0; k < todos->n; k++) idx[k] = k;
    g_tz = todos;
    qsort(idx, (size_t)todos->n, sizeof(long), cmp_tz);
    banco->n = 0;
    for (k = 0; k < todos->n; k++) {
        Tz *t = &todos->t[idx[k]];
        if (k > 0 && cmp_tz(&idx[k - 1], &idx[k]) == 0) { pos[idx[k]] = o; continue; }
        o = find_bytes(banco->p, banco->n, todos->pool.p + t->off, t->len);
        if (o < 0) {
            o = banco->n;
            buf_add(banco, todos->pool.p + t->off, t->len);
            if (banco->n > BANK_MAX) { free(idx); return 1; }
        }
        pos[idx[k]] = o;
    }
    free(idx);
    return 0;
}

static void esperas(Buf *out, long total)
{
    while (total > 0) {
        if (total <= 16) { buf_byte(out, (int)(0x70 + total - 1)); total = 0; }
        else {
            long n = total < 65535 ? total : 65535;
            buf_byte(out, 0x61); buf_u16(out, (unsigned)n);
            total -= n;
        }
    }
}

static void rearmar(const Buf *d, const Buf *datos, long loop_out, Buf *res)
{
    long doff = 0x34 + (long)rd32(d, 0x34), gd3o;
    res->n = 0;
    buf_add(res, d->p, doff < d->n ? doff : d->n);
    while (res->n < doff) buf_byte(res, 0);
    if (rd32(res, 8) < 0x161) wr32(res, 8, 0x161);
    buf_add(res, datos->p, datos->n);
    wr32(res, 0x1C, loop_out >= 0 ? (unsigned long)(doff + loop_out - 0x1C) : 0);
    gd3o = (long)rd32(d, 0x14);
    if (gd3o) {
        wr32(res, 0x14, (unsigned long)(res->n - 0x14));
        if (0x14 + gd3o < d->n) buf_add(res, d->p + 0x14 + gd3o, d->n - (0x14 + gd3o));
    }
    wr32(res, 0x04, (unsigned long)(res->n - 4));
}

static void emitir_ev(Buf *out, const Buf *d, const Ev *e)
{
    if (e->tipo == EV_N) { buf_byte(out, 0xB4); buf_byte(out, e->a); buf_byte(out, e->v); }
    else if (e->tipo == EV_P) { buf_byte(out, 0xB9); buf_byte(out, e->a); buf_byte(out, e->v); }
    else buf_add(out, d->p + e->off, e->len);
}

static int convertir_pce(const Buf *d, Buf *res, int *ncan)
{
    Evs ev = { NULL, 0, 0 };
    Wrs w[6];
    Tzs tz;
    Buf banco, out;
    long loop_idx, k, *pos = NULL, espera = 0, loop_out = -1;
    int canales[6], nc = 0, c, s, ok = 0, r = 0, sel = 0, sel_emit = -1;
    long *ini_tz;
    unsigned char *quitar;
    long frec[6];
    if (rd32(d, 8) < 0x161 || d->n < 0xB0 || rd32(d, 0xA4) == 0) return 0;
    if (parsear(d, &ev, &loop_idx)) { free(ev.e); return -1; }
    memset(w, 0, sizeof(w));
    escrituras_dda(&ev, w);
    for (c = 0; c < 6; c++) if (w[c].n >= MIN_DDA) canales[nc++] = c;
    memset(&tz, 0, sizeof(tz)); buf_init(&tz.pool);
    buf_init(&banco); buf_init(&out);
    if (!nc) goto fin;
    for (s = 0; s < 6 && !ok; s++) {
        long *cstart = (long *)xmalloc(sizeof(long) * 7);
        tz.n = 0; tz.pool.n = 0;
        for (c = 0; c < nc; c++) { cstart[c] = tz.n; trozos(&w[canales[c]], SILENCIOS[s], &tz); }
        cstart[nc] = tz.n;
        pos = (long *)xrealloc(pos, sizeof(long) * (size_t)(tz.n + 1));
        if (armar_banco(&tz, &banco, pos) == 0) {
            ok = 1;
            for (c = 0; c < nc; c++)
                for (k = cstart[c]; k < cstart[c + 1]; k++) tz.t[k].uid = canales[c];
        }
        free(cstart);
    }
    if (!ok) goto fin;
    for (c = 0; c < nc; c++) {
        frec[c] = frecuencia(&w[canales[c]]);
        if (frec[c] < 0) { r = -1; goto fin; }
    }
    ini_tz = (long *)xmalloc(sizeof(long) * (size_t)(ev.n + 1));
    quitar = (unsigned char *)xmalloc((size_t)ev.n + 1);
    for (k = 0; k < ev.n; k++) { ini_tz[k] = -1; quitar[k] = 0; }
    for (k = 0; k < tz.n; k++) ini_tz[tz.t[k].k] = k;
    for (c = 0; c < nc; c++)
        for (k = 0; k < w[canales[c]].n; k++)
            if (ini_tz[w[canales[c]].w[k].k] < 0) quitar[w[canales[c]].w[k].k] = 1;
    buf_byte(&out, 0x67); buf_byte(&out, 0x66); buf_byte(&out, 0x00);
    buf_u32(&out, (unsigned long)banco.n);
    buf_add(&out, banco.p, banco.n);
    for (c = 0; c < nc; c++) {
        unsigned char a[5];
        a[0] = 0x90; a[1] = (unsigned char)canales[c]; a[2] = 0x1B; a[3] = 0; a[4] = 6;
        buf_add(&out, a, 5);
        a[0] = 0x91; a[2] = 0; a[3] = 1; a[4] = 0;
        buf_add(&out, a, 5);
        buf_byte(&out, 0x92); buf_byte(&out, canales[c]); buf_u32(&out, (unsigned long)frec[c]);
    }
    for (k = 0; k < ev.n; k++) {
        const Ev *e = &ev.e[k];
        if (k == loop_idx) {
            esperas(&out, espera); espera = 0;
            loop_out = out.n;
            sel_emit = -1;
        }
        if (e->tipo == EV_W) { espera += e->w; continue; }
        if (quitar[k]) continue;
        esperas(&out, espera); espera = 0;
        if (ini_tz[k] >= 0) {
            const Tz *t = &tz.t[ini_tz[k]];
            buf_byte(&out, 0x93); buf_byte(&out, t->uid);
            buf_u32(&out, (unsigned long)pos[ini_tz[k]]);
            buf_byte(&out, 1);
            buf_u32(&out, (unsigned long)t->len);
        } else if (e->tipo == EV_P) {
            int a = e->a;
            if (a == 0) { sel = e->v & 7; continue; }
            if (a >= 2 && a <= 7 && sel_emit != sel) {
                buf_byte(&out, 0xB9); buf_byte(&out, 0); buf_byte(&out, sel);
                sel_emit = sel;
            }
            emitir_ev(&out, d, e);
        } else {
            emitir_ev(&out, d, e);
        }
    }
    esperas(&out, espera);
    buf_byte(&out, 0x66);
    rearmar(d, &out, loop_out, res);
    *ncan = nc;
    r = 1;
    free(ini_tz); free(quitar);
fin:
    for (c = 0; c < 6; c++) free(w[c].w);
    free(ev.e); free(pos); free(tz.t);
    buf_free(&tz.pool); buf_free(&banco); buf_free(&out);
    return r;
}

static void trozos_nes(const Wrs *w, Tzs *res)
{
    long n = w->n, ini = 0, k, med = mediana_min1(w), j;
    Buf b;
    buf_init(&b);
    while (ini < n) {
        int igual = 1;
        k = ini + 1;
        while (k < n && k - ini < NES_TROZO) {
            if (w->w[k].t - w->w[k - 1].t > 8 * med) break;
            if (w->w[k].v == w->w[k - 1].v) {
                igual++;
                if (igual >= NES_SIL) { k = k - NES_SIL + 2; break; }
            } else igual = 1;
            k++;
        }
        b.n = 0;
        for (j = ini; j < k; j++) buf_byte(&b, w->w[j].v);
        tz_add(res, ini, b.p, b.n);
        while (k < n && k > ini && w->w[k].v == w->w[k - 1].v && w->w[k].t - w->w[k - 1].t <= 8 * med) k++;
        ini = k;
    }
    buf_free(&b);
}

static int convertir_nes(const Buf *d, Buf *res)
{
    Evs ev = { NULL, 0, 0 };
    Wrs w = { NULL, 0, 0 };
    Tzs tz;
    Buf out, hist;
    long loop_idx, k, espera = 0, loop_out = -1, desde = 0, frec;
    long *ini_tz = NULL;
    unsigned char *quitar = NULL;
    int r = 0;
    if (rd32(d, 8) < 0x161 || d->n < 0x88 || rd32(d, 0x84) == 0) return 0;
    if (parsear(d, &ev, &loop_idx)) { free(ev.e); return -1; }
    escrituras_nes(&ev, &w);
    memset(&tz, 0, sizeof(tz)); buf_init(&tz.pool);
    buf_init(&out); buf_init(&hist);
    if (w.n < MIN_DDA) goto fin;
    trozos_nes(&w, &tz);
    frec = frecuencia(&w);
    if (frec < 0) { r = -1; goto fin; }
    ini_tz = (long *)xmalloc(sizeof(long) * (size_t)(ev.n + 1));
    quitar = (unsigned char *)xmalloc((size_t)ev.n + 1);
    for (k = 0; k < ev.n; k++) { ini_tz[k] = -1; quitar[k] = 0; }
    for (k = 0; k < tz.n; k++) ini_tz[w.w[tz.t[k].k].k] = k;
    for (k = 0; k < w.n; k++) if (ini_tz[w.w[k].k] < 0) quitar[w.w[k].k] = 1;
    {
        static const unsigned char a90[5] = { 0x90, 0x00, 0x14, 0x00, 0x11 };
        static const unsigned char a91[5] = { 0x91, 0x00, 0x00, 0x01, 0x00 };
        buf_add(&out, a90, 5);
        buf_add(&out, a91, 5);
        buf_byte(&out, 0x92); buf_byte(&out, 0); buf_u32(&out, (unsigned long)frec);
    }
    for (k = 0; k < ev.n; k++) {
        const Ev *e = &ev.e[k];
        if (k == loop_idx) {
            esperas(&out, espera); espera = 0;
            loop_out = out.n;
            desde = hist.n;
        }
        if (e->tipo == EV_W) { espera += e->w; continue; }
        if (quitar[k]) continue;
        esperas(&out, espera); espera = 0;
        if (ini_tz[k] >= 0) {
            const Tz *t = &tz.t[ini_tz[k]];
            const unsigned char *b = tz.pool.p + t->off;
            long base = hist.n - (NES_RING - NES_TROZO), o = -1;
            if (base < desde) base = desde;
            if (t->len >= 16) {
                o = find_bytes(hist.p + base, hist.n - base, b, t->len);
                if (o >= 0) o += base;
            }
            if (o < 0) {
                o = hist.n;
                buf_add(&hist, b, t->len);
                buf_byte(&out, 0x67); buf_byte(&out, 0x66); buf_byte(&out, 0xDF);
                buf_u32(&out, (unsigned long)(t->len + 2));
                buf_u16(&out, (unsigned)(o & (NES_RING - 1)));
                buf_add(&out, b, t->len);
            }
            buf_byte(&out, 0x93); buf_byte(&out, 0);
            buf_u32(&out, (unsigned long)(o & (NES_RING - 1)));
            buf_byte(&out, 1);
            buf_u32(&out, (unsigned long)t->len);
        } else {
            emitir_ev(&out, d, e);
        }
    }
    esperas(&out, espera);
    buf_byte(&out, 0x66);
    rearmar(d, &out, loop_out, res);
    r = 1;
fin:
    free(ev.e); free(w.w); free(tz.t); free(ini_tz); free(quitar);
    buf_free(&tz.pool); buf_free(&out); buf_free(&hist);
    return r;
}

int pred_convertir(const Buf *d, Buf *res, int *ncan)
{
    int r = convertir_pce(d, res, ncan);
    if (r == 0) { r = convertir_nes(d, res); *ncan = 1; }
    return r;
}
