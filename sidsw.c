#include "sidsw.h"

#define BLOCK 32

#define ST_REL 0
#define ST_ATT 1
#define ST_DEC 2

static const unsigned rate_tab[16] = {
    9, 32, 63, 95, 149, 220, 267, 313, 392, 977, 1954, 3126, 3907, 11720, 19532, 31251U
};

static const unsigned fc_x[9] = { 0, 8, 21, 39, 69, 99, 129, 160, 255 };
static const unsigned fc_y[9] = { 300, 330, 596, 1203, 2069, 4000, 6858, 9787, 14000U };

static unsigned fc_hz(unsigned fc)
{
    int i;

    for (i = 0; i < 8; i++)
        if (fc <= fc_x[i + 1])
            return (unsigned)(fc_y[i] + (unsigned long)(fc_y[i + 1] - fc_y[i]) * (fc - fc_x[i]) / (fc_x[i + 1] - fc_x[i]));
    return 14000U;
}

static void set_exp(SidSw *s, int v)
{
    switch (s->env[v]) {
    case 0xFF: s->exp_per[v] = 1; break;
    case 0x5D: s->exp_per[v] = 2; break;
    case 0x36: s->exp_per[v] = 4; break;
    case 0x1A: s->exp_per[v] = 8; break;
    case 0x0E: s->exp_per[v] = 16; break;
    case 0x06: s->exp_per[v] = 30; break;
    case 0x00: s->exp_per[v] = 1; break;
    }
}

static void set_rate(SidSw *s, int v)
{
    if (s->state[v] == ST_ATT) s->rate_per[v] = rate_tab[s->ad[v] >> 4];
    else if (s->state[v] == ST_DEC) s->rate_per[v] = rate_tab[s->ad[v] & 15];
    else s->rate_per[v] = rate_tab[s->sr[v] & 15];
}

static void set_filter(SidSw *s)
{
    sid_u32 f = fc_hz(s->reg[22]);
    unsigned res = s->reg[23] >> 4;
    unsigned d;

    if (f > 9800UL) f = 9800UL;
    f = f * 205888UL / s->rate;
    if (f > 12000UL) {
        s->nsub = 2;
        f >>= 1;
        if (f > 22000UL) f = 22000UL;
    } else {
        s->nsub = 1;
    }
    s->w0 = (int)f;
    d = 181 + (res * 256U + 7) / 15;
    s->q1 = (int)(4194304UL / d);
    s->gain = (unsigned)(s->reg[24] & 15) * 32U;
}

void sidsw_init(SidSw *s, unsigned long clock, unsigned long sample_rate)
{
    unsigned char *p = (unsigned char *)s;
    unsigned i;

    for (i = 0; i < sizeof(SidSw); i++) p[i] = 0;
    s->clock = clock;
    s->rate = sample_rate;
    s->cps_q8 = (clock << 8) / sample_rate;
    for (i = 0; i < 3; i++) {
        s->lfsr[i] = 0x7FFFF8UL;
        s->exp_per[i] = 1;
        s->rate_per[i] = rate_tab[0];
    }
    set_filter(s);
}

void sidsw_write_reg(SidSw *s, unsigned reg, unsigned char data)
{
    int v;
    unsigned char old;

    reg &= 31;
    s->reg[reg] = data;
    if (reg >= 21) {
        set_filter(s);
        return;
    }
    v = (int)(reg / 7);
    switch (reg % 7) {
    case 0:
    case 1:
        s->freq[v] = s->reg[v * 7] | ((unsigned)s->reg[v * 7 + 1] << 8);
        s->inc[v] = (unsigned long)s->freq[v] * s->cps_q8;
        break;
    case 2:
    case 3:
        s->pw[v] = s->reg[v * 7 + 2] | ((unsigned)(s->reg[v * 7 + 3] & 15) << 8);
        break;
    case 4:
        old = s->ctrl[v];
        s->ctrl[v] = data;
        if ((data & 1) && !(old & 1)) {
            s->state[v] = ST_ATT;
            set_rate(s, v);
        } else if (!(data & 1) && (old & 1)) {
            s->state[v] = ST_REL;
            set_rate(s, v);
        }
        if ((old & 8) && !(data & 8)) s->lfsr[v] = 0x7FFFF8UL;
        break;
    case 5:
        s->ad[v] = data;
        set_rate(s, v);
        break;
    case 6:
        s->sr[v] = data;
        set_rate(s, v);
        break;
    }
}

static void env_update(SidSw *s, int v, sid_u32 cyc)
{
    unsigned per;
    unsigned char sus;

    sus = (unsigned char)((s->sr[v] >> 4) * 17);
    if (s->state[v] == ST_DEC ? s->env[v] <= sus : (s->state[v] == ST_REL && s->env[v] == 0)) {
        s->rate_cnt[v] = 0;
        return;
    }
    s->rate_cnt[v] += cyc;
    per = s->rate_per[v];
    while (s->rate_cnt[v] >= per) {
        s->rate_cnt[v] -= per;
        if (s->state[v] == ST_ATT) {
            if (s->env[v] < 255) s->env[v]++;
            if (s->env[v] == 255) {
                s->state[v] = ST_DEC;
                set_rate(s, v);
                per = s->rate_per[v];
            }
            set_exp(s, v);
        } else {
            if (++s->exp_cnt[v] >= s->exp_per[v]) {
                s->exp_cnt[v] = 0;
                if (s->state[v] == ST_DEC) {
                    if (s->env[v] > sus) { s->env[v]--; set_exp(s, v); }
                } else if (s->env[v] > 0) {
                    s->env[v]--;
                    set_exp(s, v);
                }
            }
            if (s->state[v] == ST_DEC ? s->env[v] <= sus : s->env[v] == 0) {
                s->rate_cnt[v] = 0;
                break;
            }
        }
    }
}

static unsigned char noise8(unsigned long l)
{
    return (unsigned char)(((l >> 15) & 0x80) | ((l >> 13) & 0x40) | ((l >> 10) & 0x20) | ((l >> 7) & 0x10) |
                           ((l >> 4) & 0x08) | ((l >> 2) & 0x04) | ((l >> 1) & 0x02) | (l & 0x01));
}

#define T_OFF   0
#define T_TRI   1
#define T_SAW   2
#define T_PUL   3
#define T_NOI   4
#define T_SLOW  5
#define T_RING  6

static signed char tri_tab[256];
static signed char saw_tab[256];
static int tri_ready;

static void tri_build(void)
{
    unsigned x, y;

    for (x = 0; x < 256; x++) {
        y = (x & 0x80) ? (~x & 0xFF) : x;
        tri_tab[x] = (signed char)(((y << 1) & 0xFF) - 128);
        saw_tab[x] = (signed char)((int)x - 128);
    }
    tri_ready = 1;
}

static int sat(sid_s32 x)
{
    return (int)(x > 32767 ? 32767 : x < -32768L ? -32767 : x);
}

#define HI16(x) ((int)((sid_s32)(x) >> 16))

#define FILT1(fin) do { \
    int t_ = HI16((sid_s32)bp * q1) * 2; \
    hp = sat((sid_s32)sat((sid_s32)sat((sid_s32)(fin) - lp) - t_) - t_); \
    t_ = HI16((sid_s32)hp * w0) * 2; \
    bp = sat((sid_s32)bp + t_); \
    t_ = HI16((sid_s32)bp * w0) * 2; \
    lp = sat((sid_s32)lp + t_); \
} while (0)

#define FILT(fin) do { \
    int sub_; \
    for (sub_ = 0; sub_ < nsub; sub_++) FILT1(fin); \
} while (0)

#define OUTMIX() do { \
    if (mode & 0x10) fout = sat((sid_s32)fout + lp); \
    if (mode & 0x20) fout = sat((sid_s32)fout + bp); \
    if (mode & 0x40) fout = sat((sid_s32)fout + hp); \
} while (0)

#define OUTPUT() do { \
    t = HI16((sid_s32)fout * (sid_s32)gain) + 128; \
    *out++ = (unsigned char)(t < 0 ? 0 : t > 255 ? 255 : t); \
} while (0)

#define NOISE_STEP(k) do { \
    unsigned hn_ = (unsigned)(a##k >> 16); \
    unsigned c0_ = (ho##k >> 11) & 31, d_ = ((hn_ >> 11) - c0_) & 31; \
    d_ = ((c0_ + d_ + 1) >> 1) - ((c0_ + 1) >> 1); \
    while (d_--) { \
        l##k = ((l##k << 1) | (((l##k >> 22) ^ (l##k >> 17)) & 1)) & 0x7FFFFFUL; \
        nz##k = noise8(l##k); \
    } \
} while (0)

#define ACC(k) do { \
    ho##k = (unsigned)(a##k >> 16); \
    a##k += i##k; \
} while (0)

#define VOICE(k, src) do { \
    if (ty##k) { \
        if (ty##k == T_PUL) o = ((unsigned)(a##k >> 16) >= pw##k) ? ph##k : pl##k; \
        else if (ty##k == T_NOI) { NOISE_STEP(k); o = ((int)nz##k - 128) * e##k; } \
        else if (ty##k == T_RING) o = tb##k[(unsigned)((a##k >> 24) ^ ((a##src >> 24) & 0x80))] * e##k; \
        else o = tb##k[(unsigned)(a##k >> 24)] * e##k; \
        if (r##k) fin += o; else fout += o; \
    } \
} while (0)

#if defined(__WATCOMC__) || defined(SID_ASM)
#ifndef SID_U16
#define SID_U16 unsigned
#endif
#ifndef SID_TBOFF
#define SID_TBOFF(p) ((unsigned)(p))
#endif
#ifndef SID_OUTSET
#include <dos.h>
#define SID_OUTSET(f, o) ((f).out_off = FP_OFF(o), (f).out_seg = FP_SEG(o))
#endif

typedef struct {
    SID_U16 acclo, acchi, inclo, inchi, ty, pw, ph, pl, e, tb, lfsrlo, lfsrhi, nz, oldhi;
} SfVoice;

typedef struct {
    SID_U16 out_off, out_seg, n, flags, lp, bp, hp, w0, q1, nsub, gain;
    SfVoice v[3];
} SidFast;

void sid_fast(SidFast *p);

static SidFast sf;

static void run_fast(SidSw *s, unsigned char far *out, int n)
{
    int k, e, mode = s->reg[24] & 0x70;
    SfVoice *p;

    SID_OUTSET(sf, out);
    sf.n = (SID_U16)n;
    sf.flags = (SID_U16)((s->reg[23] & 7) | mode | (((s->reg[23] & 7) || mode) ? 0x80 : 0));
    sf.lp = (SID_U16)s->lp; sf.bp = (SID_U16)s->bp; sf.hp = (SID_U16)s->hp;
    sf.w0 = (SID_U16)s->w0; sf.q1 = (SID_U16)s->q1; sf.nsub = (SID_U16)s->nsub; sf.gain = (SID_U16)s->gain;
    for (k = 0; k < 3; k++) {
        p = &sf.v[k];
        if (s->ctrl[k] & 8) {
            p->acclo = p->acchi = p->inclo = p->inchi = 0;
        } else {
            p->acclo = (SID_U16)s->acc[k]; p->acchi = (SID_U16)(s->acc[k] >> 16);
            p->inclo = (SID_U16)s->inc[k]; p->inchi = (SID_U16)(s->inc[k] >> 16);
        }
        switch (s->ty[k]) {
        case T_TRI: p->ty = 2; p->tb = (SID_U16)SID_TBOFF(tri_tab); break;
        case T_RING: p->ty = 8; p->tb = (SID_U16)SID_TBOFF(tri_tab); break;
        case T_SAW: p->ty = 2; p->tb = (SID_U16)SID_TBOFF(saw_tab); break;
        case T_PUL: p->ty = 4; break;
        case T_NOI: p->ty = 6; break;
        default: p->ty = 0; break;
        }
        if (k == 2 && (s->reg[24] & 0x80) && !(s->reg[23] & 4)) p->ty = 0;
        e = s->env[k] >> 2;
        p->e = (SID_U16)e;
        p->pw = (SID_U16)(s->pw[k] << 4);
        p->ph = (SID_U16)(e * 127);
        p->pl = (SID_U16)(e * -128);
        p->lfsrlo = (SID_U16)s->lfsr[k]; p->lfsrhi = (SID_U16)(s->lfsr[k] >> 16);
        p->nz = s->noise_out[k];
    }
    sid_fast(&sf);
    for (k = 0; k < 3; k++) {
        p = &sf.v[k];
        s->acc[k] = ((sid_u32)p->acchi << 16) | p->acclo;
        s->lfsr[k] = ((sid_u32)p->lfsrhi << 16) | p->lfsrlo;
        s->noise_out[k] = (unsigned char)p->nz;
    }
    s->lp = (int)(short)sf.lp; s->bp = (int)(short)sf.bp; s->hp = (int)(short)sf.hp;
}
#else
static void run_fast(SidSw *s, unsigned char far *out, int n)
{
    sid_u32 a0 = s->acc[0], a1 = s->acc[1], a2 = s->acc[2];
    sid_u32 i0 = s->inc[0], i1 = s->inc[1], i2 = s->inc[2];
    sid_u32 l0 = s->lfsr[0], l1 = s->lfsr[1], l2 = s->lfsr[2];
    unsigned ho0, ho1, ho2;
    unsigned pw0 = s->pw[0] << 4, pw1 = s->pw[1] << 4, pw2 = s->pw[2] << 4;
    unsigned char ty0 = s->ty[0], ty1 = s->ty[1], ty2 = s->ty[2];
    unsigned char nz0 = s->noise_out[0], nz1 = s->noise_out[1], nz2 = s->noise_out[2];
    int e0 = s->env[0] >> 2, e1 = s->env[1] >> 2, e2 = s->env[2] >> 2;
    int ph0 = e0 * 127, ph1 = e1 * 127, ph2 = e2 * 127;
    int pl0 = e0 * -128, pl1 = e1 * -128, pl2 = e2 * -128;
    const signed char *tb0 = s->ty[0] == T_SAW ? saw_tab : tri_tab;
    const signed char *tb1 = s->ty[1] == T_SAW ? saw_tab : tri_tab;
    const signed char *tb2 = s->ty[2] == T_SAW ? saw_tab : tri_tab;
    int r0 = s->reg[23] & 1, r1 = s->reg[23] & 2, r2 = s->reg[23] & 4;
    int mode = s->reg[24] & 0x70, filt = (s->reg[23] & 7) || mode;
    int mute3 = (s->reg[24] & 0x80) && !r2;
    int lp = s->lp, bp = s->bp, hp = s->hp, w0 = s->w0, q1 = s->q1, nsub = s->nsub;
    unsigned gain = s->gain;
    int fin, fout, o, i, t;

    if (mute3) ty2 = T_OFF;
    if (s->ctrl[0] & 8) { a0 = 0; i0 = 0; }
    if (s->ctrl[1] & 8) { a1 = 0; i1 = 0; }
    if (s->ctrl[2] & 8) { a2 = 0; i2 = 0; }
    for (i = 0; i < n; i++) {
        fin = 0;
        fout = 0;
        ACC(0);
        ACC(1);
        ACC(2);
        VOICE(0, 2);
        VOICE(1, 0);
        VOICE(2, 1);
        if (filt) {
            FILT(fin);
            OUTMIX();
        }
        OUTPUT();
    }
    s->acc[0] = a0; s->acc[1] = a1; s->acc[2] = a2;
    s->lfsr[0] = l0; s->lfsr[1] = l1; s->lfsr[2] = l2;
    s->noise_out[0] = nz0; s->noise_out[1] = nz1; s->noise_out[2] = nz2;
    s->lp = lp; s->bp = bp; s->hp = hp;
}

#endif

static int classify(SidSw *s)
{
    int v, slow = 0;
    unsigned char c, w;

    for (v = 0; v < 3; v++) {
        c = s->ctrl[v];
        w = c & 0xF0;
        if (!s->env[v] || !w) s->ty[v] = T_OFF;
        else if (c & 0x02) { s->ty[v] = T_SLOW; slow = 1; }
        else if (w == 0x10) s->ty[v] = (c & 0x04) ? T_RING : T_TRI;
        else if (w == 0x20) s->ty[v] = T_SAW;
        else if (w == 0x40) s->ty[v] = T_PUL;
        else if (w == 0x80) s->ty[v] = T_NOI;
        else { s->ty[v] = T_SLOW; slow = 1; }
    }
    return slow;
}

static void run_slow(SidSw *s, unsigned char far *out, int n)
{
    int v, i, fin, fout, t, w, o, mode, filt_on;
    int lp = s->lp, bp = s->bp, hp = s->hp, w0 = s->w0, q1 = s->q1, nsub = s->nsub;
    unsigned gain = s->gain;
    sid_u32 old[3], c0, d;
    unsigned wrapped[3];
    unsigned char ctrl;

    mode = s->reg[24] & 0x70;
    filt_on = s->reg[23] & 7;
    for (i = 0; i < n; i++) {
        for (v = 0; v < 3; v++) {
            old[v] = s->acc[v];
            if (s->ctrl[v] & 8) s->acc[v] = 0;
            else s->acc[v] += s->inc[v];
            wrapped[v] = s->acc[v] < old[v];
            if ((s->ctrl[v] & 0x80) && !(s->ctrl[v] & 8)) {
                c0 = old[v] >> 27;
                d = ((s->acc[v] >> 27) - c0) & 31;
                d = ((c0 + d + 1) >> 1) - ((c0 + 1) >> 1);
                while (d--) {
                    s->lfsr[v] = ((s->lfsr[v] << 1) | (((s->lfsr[v] >> 22) ^ (s->lfsr[v] >> 17)) & 1)) & 0x7FFFFFUL;
                    s->noise_out[v] = noise8(s->lfsr[v]);
                }
            }
        }
        for (v = 0; v < 3; v++)
            if ((s->ctrl[v] & 2) && wrapped[(v + 2) % 3]) s->acc[v] = 0;
        fin = 0;
        fout = 0;
        for (v = 0; v < 3; v++) {
            ctrl = s->ctrl[v];
            if (!s->env[v] || !(ctrl & 0xF0)) continue;
            w = 255;
            if (ctrl & 0x10) {
                sid_u32 a = s->acc[v];
                unsigned x;

                if ((ctrl & 4) && (s->acc[(v + 2) % 3] & 0x80000000UL)) a ^= 0x80000000UL;
                x = (unsigned)(a >> 24);
                if (x & 0x80) x = ~x;
                w &= (int)((x << 1) & 0xFF);
            }
            if (ctrl & 0x20) w &= (int)(s->acc[v] >> 24);
            if (ctrl & 0x40) w &= ((unsigned)(s->acc[v] >> 20) >= s->pw[v]) ? 255 : 0;
            if (ctrl & 0x80) w &= s->noise_out[v];
            o = (int)(((w - 128) * (int)(s->env[v] >> 2)));
            if (s->reg[23] & (1 << v)) fin += o;
            else if (v != 2 || !(s->reg[24] & 0x80)) fout += o;
        }
        if (filt_on || mode) {
            FILT(fin);
            OUTMIX();
        }
        OUTPUT();
    }
    s->lp = lp; s->bp = bp; s->hp = hp;
}

void sidsw_run(SidSw *s, unsigned char far *out, unsigned long count)
{
    int v, n, slow;

    if (!tri_ready) tri_build();
    while (count) {
        if (s->blk_left == 0) {
            for (v = 0; v < 3; v++) env_update(s, v, (sid_u32)BLOCK * s->cps_q8 >> 8);
            s->blk_left = BLOCK;
        }
        n = s->blk_left;
        if ((unsigned long)n > count) n = (int)count;
        s->blk_left -= n;
        count -= n;
        slow = classify(s);
        if (slow) run_slow(s, out, n);
        else run_fast(s, out, n);
        out += n;
    }
}
