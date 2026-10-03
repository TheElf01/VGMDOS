#include <string.h>
#include "ay8910.h"
#include "dosmem.h"

static const unsigned char vol_table_ay[16] = {
    0, 0, 0, 1, 1, 1, 2, 3, 4, 5, 7, 10, 14, 20, 28, 40
};
static const unsigned char env_table_ay[32] = {
    0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 2, 2, 2, 3,
    3, 4, 4, 5, 6, 7, 8, 10, 12, 14, 17, 20, 24, 28, 34, 40
};

static const unsigned char vol_table_ym[16] = {
    0, 0, 1, 1, 1, 2, 2, 3, 4, 6, 8, 11, 16, 21, 30, 40
};
static const unsigned char env_table_ym[32] = {
    0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 2, 2, 2, 3, 3,
    4, 4, 5, 6, 7, 8, 10, 11, 13, 16, 19, 21, 25, 30, 35, 40
};

#define FIXED_SHIFT 8
#define FIXED_ONE (1UL << FIXED_SHIFT)

void ay8910_init(Ay8910 *a, unsigned long clock, unsigned long sample_rate, int chip_type)
{
    memset(a, 0, sizeof(*a));
    a->clock = clock;
    a->sample_rate = sample_rate;
    a->vol_table = (chip_type == AY8910_CHIP_YM2149) ? vol_table_ym : vol_table_ay;
    a->env_table = (chip_type == AY8910_CHIP_YM2149) ? env_table_ym : env_table_ay;
    a->noise_shift = 0x1FFFF;
    a->tq8 = (clock << FIXED_SHIFT) / (8UL * sample_rate);
    a->c_per[0] = a->c_per[1] = a->c_per[2] = 0xFFFF;
    a->c_np = 0xFF; a->c_ep = 0xFFFF;
    a->mixer = 0x3F;
    a->tone_counter[0] = a->tone_counter[1] = a->tone_counter[2] = 0;
    a->noise_counter = 0;
    a->env_counter = 0;
}

static void env_reset(Ay8910 *a)
{
    a->env_counter = 0;
    a->env_holding = 0;
    if (a->env_shape & 0x04) {
        a->env_level = 0;
        a->env_dir = 1;
    } else {
        a->env_level = 31;
        a->env_dir = 0;
    }
}

void ay8910_write_reg(Ay8910 *a, unsigned reg, unsigned char data)
{
    int ch;
    switch (reg) {
        case 0:
        case 2:
        case 4:
            ch = reg >> 1;
            a->tone_period[ch] = (unsigned short)((a->tone_period[ch] & 0x0F00) | data);
            break;
        case 1:
        case 3:
        case 5:
            ch = reg >> 1;
            a->tone_period[ch] = (unsigned short)((a->tone_period[ch] & 0x00FF) | ((unsigned short)(data & 0x0F) << 8));
            break;
        case 6:
            a->noise_period = data & 0x1F;
            break;
        case 7:
            a->mixer = data;
            break;
        case 8:
        case 9:
        case 10:
            a->vol_reg[reg - 8] = data & 0x1F;
            break;
        case 11:
            a->env_period = (unsigned short)((a->env_period & 0xFF00) | data);
            break;
        case 12:
            a->env_period = (unsigned short)((a->env_period & 0x00FF) | ((unsigned short)data << 8));
            break;
        case 13:
            a->env_shape = data & 0x0F;
            env_reset(a);
            break;
        default:
            break;
    }
}

#define AY_NTAB 32767U
static DosBuffer ay_nbuf;
static unsigned char far *ay_ntab = 0;
static int ay_ntab_tried = 0;

static unsigned char ay_envtab[256];
static const Ay8910 *ay_env_owner = 0;
static int ay_env_key = -1;
static unsigned char ay_cm[3][512];
static int ay_cm_key = -1;

static unsigned int a_ph0, a_ph1, a_ph2, a_inc0, a_inc1, a_inc2;
static unsigned int a_nacc, a_nstep, a_nidx, a_nseg, a_n;
static unsigned char a_nbit, a_amp0, a_amp1, a_amp2, a_envon, a_envm, a_base, a_non;
static unsigned int a_eph_lo, a_eph_hi, a_einc_lo, a_einc_hi;
static unsigned char far *a_out;
static unsigned int a_oseg;

static void ay_build_noise(void)
{
    unsigned long sh = 0x1FFFFUL;
    unsigned int i;
    ay_ntab_tried = 1;
    if (!dosmem_alloc_simple(&ay_nbuf, AY_NTAB)) return;
    ay_ntab = ay_nbuf.ptr;
    for (i = 0; i < AY_NTAB; i++) {
        unsigned int fb = ((unsigned int)sh & 1) ^ (((unsigned int)sh >> 3) & 1);
        ay_ntab[i] = (unsigned char)(sh & 1);
        sh = (sh >> 1) | ((unsigned long)fb << 16);
    }
}

static int env_level_at(unsigned char shape, int pos)
{
    int att = (shape & 4) != 0, up;
    if (!(shape & 8)) return pos < 32 ? (att ? pos : 31 - pos) : 0;
    if (pos < 32) return att ? pos : 31 - pos;
    pos -= 32;
    if (shape & 1) {
        up = att;
        if (shape & 2) up = !up;
        return up ? 31 : 0;
    }
    up = att;
    if (shape & 2) up = !up;
    return up ? pos : 31 - pos;
}

void ay8910_set_gain(Ay8910 *a, unsigned g256)
{
    int k;
    for (k = 0; k < 16; k++) a->vt_s[k] = (unsigned char)((a->vol_table[k] * g256 + 128) >> 8);
    for (k = 0; k < 32; k++) a->et_s[k] = (unsigned char)((a->env_table[k] * g256 + 128) >> 8);
    a->vol_table = a->vt_s;
    a->env_table = a->et_s;
    ay_env_owner = 0;
}

static void ay_env_table(const Ay8910 *a)
{
    int key = a->env_shape;
    int k;
    if (ay_env_owner == a && ay_env_key == key) return;
    for (k = 0; k < 256; k++) ay_envtab[k] = a->env_table[env_level_at(a->env_shape, k >> 2)];
    ay_env_owner = a;
    ay_env_key = key;
}

static void ay_mix_asm(void)
{
    _asm {
        push bx
        push cx
        push dx
        push es
        push si
        push di
        mov cx, a_n
        les di, a_out
        mov si, a_ph0
        mov dx, a_ph1
        mov ax, a_ph2
        push bp
        mov bp, ax
        xor bx, bx
    alp:
        cmp a_non, 0
        je  anoz
        mov bx, a_nacc
        add bx, a_nstep
        mov byte ptr a_nacc, bl
        mov bl, bh
        xor bh, bh
        add bx, a_nidx
        cmp bx, 32767
        jb  anok
        sub bx, 32767
    anok:
        mov a_nidx, bx
        mov ax, a_nseg
        mov es, ax
        mov al, es:[bx]
        mov a_nbit, al
        mov ax, a_oseg
        mov es, ax
    anoz:
        cmp a_envon, 0
        je  aenz
        mov ax, a_einc_lo
        add a_eph_lo, ax
        mov ax, a_einc_hi
        adc ax, a_eph_hi
        mov a_eph_hi, ax
        mov bl, ah
        xor bh, bh
        mov al, ay_envtab[bx]
        test a_envm, 1
        jz  ae1
        mov a_amp0, al
    ae1:
        test a_envm, 2
        jz  ae2
        mov a_amp1, al
    ae2:
        test a_envm, 4
        jz  aenz
        mov a_amp2, al
    aenz:
        mov al, a_base
        add si, a_inc0
        mov bx, si
        mov bl, bh
        mov bh, a_nbit
        mov ah, ay_cm[bx]
        and ah, a_amp0
        add al, ah
        add dx, a_inc1
        mov bx, dx
        mov bl, bh
        mov bh, a_nbit
        mov ah, ay_cm+512[bx]
        and ah, a_amp1
        add al, ah
        add bp, a_inc2
        mov bx, bp
        mov bl, bh
        mov bh, a_nbit
        mov ah, ay_cm+1024[bx]
        and ah, a_amp2
        add al, ah
        stosb
        dec cx
        jz  adone
        jmp alp
    adone:
        mov ax, bp
        pop bp
        mov a_ph2, ax
        mov a_ph0, si
        mov a_ph1, dx
        pop di
        pop si
        pop es
        pop dx
        pop cx
        pop bx
    }
}

void ay8910_run(Ay8910 *a, unsigned char far *out, unsigned long count)
{
    unsigned long tq8 = a->tq8;
    unsigned char mixer = a->mixer;
    int ch;
    unsigned char vr[3];
    int env_used;
    vr[0] = (a->mute & 1) ? 0 : a->vol_reg[0];
    vr[1] = (a->mute & 2) ? 0 : a->vol_reg[1];
    vr[2] = (a->mute & 4) ? 0 : a->vol_reg[2];
    env_used = ((vr[0] | vr[1] | vr[2]) & 0x10) != 0;
    if (!ay_ntab_tried) ay_build_noise();
    for (ch = 0; ch < 3; ch++) {
        unsigned int p = a->tone_period[ch];
        unsigned int ph = (unsigned int)a->tone_counter[ch];
        if (p != a->c_per[ch]) {
            unsigned int inc = 0;
            if (p > 7) {
                unsigned long v = (tq8 << 7) / p;
                inc = v > 0x7FFFUL ? 0x7FFF : (unsigned int)v;
            }
            a->c_per[ch] = (unsigned short)p;
            a->c_inc[ch] = (unsigned short)inc;
        }
        if (p <= 7) ph &= 0x7FFF;
        if (ch == 0) { a_ph0 = ph; a_inc0 = a->c_inc[0]; }
        else if (ch == 1) { a_ph1 = ph; a_inc1 = a->c_inc[1]; }
        else { a_ph2 = ph; a_inc2 = a->c_inc[2]; }
    }
    if (ay_cm_key != mixer) for (ay_cm_key = mixer, ch = 0; ch < 3; ch++) {
        int tf = (mixer >> ch) & 1, nf = (mixer >> (ch + 3)) & 1, n;
        for (n = 0; n < 2; n++) {
            unsigned char lo = (unsigned char)(((0 | tf) & (n | nf)) ? 0xFF : 0);
            unsigned char hi = (unsigned char)(((1 | tf) & (n | nf)) ? 0xFF : 0);
            memset(ay_cm[ch] + (n << 8), lo, 128);
            memset(ay_cm[ch] + (n << 8) + 128, hi, 128);
        }
    }
    a_non = 0;
    a_nbit = (unsigned char)(a->noise_output & 1);
    a_nidx = (unsigned int)a->noise_counter;
    if (a_nidx >= AY_NTAB) a_nidx = 0;
    a_nacc = (unsigned int)(a->clock_accum & 0xFF);
    if (a->noise_period && ay_ntab && (mixer & 0x38) != 0x38) {
        if (a->c_np != a->noise_period) {
            unsigned long st = tq8 / a->noise_period;
            a->c_nstep = st > 0x7F00UL ? 0x7F00U : (unsigned int)st;
            a->c_np = a->noise_period;
        }
        a_nstep = a->c_nstep;
        a_nseg = FP_SEG(ay_ntab) + (FP_OFF(ay_ntab) >> 4);
        if ((FP_OFF(ay_ntab) & 15) == 0) a_non = 1;
    }
    a_amp0 = a->vol_table[vr[0] & 0x0F];
    a_amp1 = a->vol_table[vr[1] & 0x0F];
    a_amp2 = a->vol_table[vr[2] & 0x0F];
    a_envm = (unsigned char)(((vr[0] >> 4) & 1) | ((vr[1] >> 3) & 2) | ((vr[2] >> 2) & 4));
    a_envon = 0;
    a_base = 68;
    if (!env_used) {
        unsigned char amp[3];
        int k, cst = 1, sum = 68;
        amp[0] = a_amp0; amp[1] = a_amp1; amp[2] = a_amp2;
        for (k = 0; k < 3; k++) {
            if (!amp[k]) continue;
            if (((mixer >> k) & 1) && ((mixer >> (k + 3)) & 1)) sum += amp[k];
            else cst = 0;
        }
        if (cst && sum < 256) {
            while (count) {
                unsigned n = count > 60000UL ? 60000U : (unsigned)count;
                _fmemset(out, sum, n);
                out += n; count -= n;
            }
            return;
        }
    }
    a_eph_lo = (unsigned int)(a->env_counter & 0xFFFF);
    a_eph_hi = (unsigned int)((unsigned long)a->env_counter >> 16);
    a_einc_lo = a_einc_hi = 0;
    if (env_used) {
        unsigned long ei;
        ay_env_table(a);
        if (a->c_ep != a->env_period) {
            if (a->env_period <= 4) ei = 1024UL << 16;
            else {
                ei = ((tq8 << 17) / a->env_period) << 1;
                if (ei > (1024UL << 16)) ei = 1024UL << 16;
            }
            a->c_einc = ei;
            a->c_ep = a->env_period;
        }
        ei = a->c_einc;
        if (!a->env_holding) { a_einc_lo = (unsigned int)(ei & 0xFFFF); a_einc_hi = (unsigned int)(ei >> 16); }
        {
            unsigned char e = ay_envtab[a_eph_hi >> 8];
            if (a_envm & 1) a_amp0 = e;
            if (a_envm & 2) a_amp1 = e;
            if (a_envm & 4) a_amp2 = e;
        }
        a_envon = 1;
    }
    while (count) {
        unsigned int n = count > 60000UL ? 60000U : (unsigned int)count;
        int hold_shape = a_envon && (!(a->env_shape & 8) || (a->env_shape & 1)) && !a->env_holding;
        if (hold_shape && n > 32) n = 32;
        count -= n;
        a_out = out; a_n = n; a_oseg = FP_SEG(out);
        ay_mix_asm();
        out += n;
        if (hold_shape && (a_eph_hi >> 10) >= 32) {
            a->env_holding = 1;
            a_eph_hi = 32U << 10; a_eph_lo = 0;
            a_einc_lo = a_einc_hi = 0;
        }
    }
    a->tone_counter[0] = (long)a_ph0;
    a->tone_counter[1] = (long)a_ph1;
    a->tone_counter[2] = (long)a_ph2;
    a->noise_counter = (long)a_nidx;
    a->noise_output = a_nbit;
    a->clock_accum = a_nacc;
    a->env_counter = (long)(((unsigned long)a_eph_hi << 16) | a_eph_lo);
    a->env_level = (unsigned char)env_level_at(a->env_shape, (a_eph_hi >> 10) & 63);
}
