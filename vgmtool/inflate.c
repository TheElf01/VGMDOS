#include <setjmp.h>
#include <string.h>
#include "vgmlib.h"

typedef struct {
    const unsigned char *in;
    long n, pos;
    unsigned long bb;
    int bc;
    Buf *out;
    jmp_buf err;
} St;

typedef struct {
    short count[16];
    short sym[320];
} Huff;

static int bits(St *s, int need)
{
    unsigned long v = s->bb;
    while (s->bc < need) {
        if (s->pos >= s->n) longjmp(s->err, 1);
        v |= (unsigned long)s->in[s->pos++] << s->bc;
        s->bc += 8;
    }
    s->bb = v >> need;
    s->bc -= need;
    return (int)(v & ((1UL << need) - 1));
}

static int decode(St *s, const Huff *h)
{
    int code = 0, first = 0, index = 0, len, count;
    for (len = 1; len <= 15; len++) {
        code |= bits(s, 1);
        count = h->count[len];
        if (code - count < first) return h->sym[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    longjmp(s->err, 2);
    return 0;
}

static void construct(Huff *h, const short *length, int n)
{
    short offs[16];
    int sym, len;
    for (len = 0; len < 16; len++) h->count[len] = 0;
    for (sym = 0; sym < n; sym++) h->count[length[sym]]++;
    offs[1] = 0;
    for (len = 1; len < 15; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (sym = 0; sym < n; sym++)
        if (length[sym]) h->sym[offs[length[sym]]++] = (short)sym;
}

static const short lbase[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
    35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const short lext[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const short dbase[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
    257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const short dext[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static void codes(St *s, const Huff *lc, const Huff *dc)
{
    int sym, len;
    long dist, k;
    Buf *o = s->out;
    for (;;) {
        sym = decode(s, lc);
        if (sym < 256) { buf_byte(o, sym); continue; }
        if (sym == 256) return;
        sym -= 257;
        if (sym >= 29) longjmp(s->err, 3);
        len = lbase[sym] + bits(s, lext[sym]);
        sym = decode(s, dc);
        if (sym >= 30) longjmp(s->err, 3);
        dist = dbase[sym] + bits(s, dext[sym]);
        if (dist > o->n) longjmp(s->err, 4);
        buf_reserve(o, o->n + len);
        for (k = 0; k < len; k++) { o->p[o->n] = o->p[o->n - dist]; o->n++; }
    }
}

static void fixed(St *s)
{
    static Huff lc, dc;
    static int hecho = 0;
    if (!hecho) {
        short l[288];
        int i;
        for (i = 0; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        construct(&lc, l, 288);
        for (i = 0; i < 30; i++) l[i] = 5;
        construct(&dc, l, 30);
        hecho = 1;
    }
    codes(s, &lc, &dc);
}

static void dynamic(St *s)
{
    static const short order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
    short len[320];
    Huff lc, dc;
    int nlen, ndist, ncode, idx, sym, rep, val;
    nlen = bits(s, 5) + 257;
    ndist = bits(s, 5) + 1;
    ncode = bits(s, 4) + 4;
    if (nlen > 286 || ndist > 30) longjmp(s->err, 5);
    for (idx = 0; idx < 19; idx++) len[order[idx]] = 0;
    for (idx = 0; idx < ncode; idx++) len[order[idx]] = (short)bits(s, 3);
    construct(&lc, len, 19);
    idx = 0;
    while (idx < nlen + ndist) {
        sym = decode(s, &lc);
        if (sym < 16) { len[idx++] = (short)sym; continue; }
        if (sym == 16) {
            if (idx == 0) longjmp(s->err, 5);
            val = len[idx - 1];
            rep = 3 + bits(s, 2);
        } else if (sym == 17) {
            val = 0; rep = 3 + bits(s, 3);
        } else {
            val = 0; rep = 11 + bits(s, 7);
        }
        if (idx + rep > nlen + ndist) longjmp(s->err, 5);
        while (rep--) len[idx++] = (short)val;
    }
    construct(&lc, len, nlen);
    construct(&dc, len + nlen, ndist);
    codes(s, &lc, &dc);
}

static void stored(St *s)
{
    unsigned len;
    s->bb = 0; s->bc = 0;
    if (s->pos + 4 > s->n) longjmp(s->err, 6);
    len = s->in[s->pos] | (s->in[s->pos + 1] << 8);
    s->pos += 4;
    if (s->pos + (long)len > s->n) longjmp(s->err, 6);
    buf_add(s->out, s->in + s->pos, len);
    s->pos += len;
}

int gunzip(const unsigned char *in, long n, Buf *out)
{
    St s;
    long p = 0;
    out->n = 0;
    if (n >= 4) buf_reserve(out, (long)(in[n - 4] | (in[n - 3] << 8) | ((long)in[n - 2] << 16) | ((long)in[n - 1] << 24)) & 0x7FFFFFFL);
    while (p + 18 <= n && in[p] == 0x1F && in[p + 1] == 0x8B) {
        int flg = in[p + 3], last;
        if (in[p + 2] != 8) return 1;
        p += 10;
        if (flg & 4) { if (p + 2 > n) return 1; p += 2 + (in[p] | (in[p + 1] << 8)); }
        if (flg & 8) { while (p < n && in[p]) p++; p++; }
        if (flg & 16) { while (p < n && in[p]) p++; p++; }
        if (flg & 2) p += 2;
        if (p >= n) return 1;
        s.in = in; s.n = n; s.pos = p; s.bb = 0; s.bc = 0; s.out = out;
        if (setjmp(s.err)) return 1;
        do {
            int t;
            last = bits(&s, 1);
            t = bits(&s, 2);
            if (t == 0) stored(&s);
            else if (t == 1) fixed(&s);
            else if (t == 2) dynamic(&s);
            else longjmp(s.err, 7);
        } while (!last);
        p = s.pos + 8;
    }
    return 0;
}
