#include <string.h>
#include <math.h>
#include "fds.h"

#define RC_BITS 12

#ifdef __WATCOMC__

unsigned long fast_mul16(unsigned int a, unsigned int b);
#pragma aux fast_mul16 = \
    "mul dx" \
    parm [ax] [dx] \
    value [dx ax];
#else

static unsigned long fast_mul16(unsigned int a, unsigned int b)
{
    return (unsigned long)a * (unsigned long)b;
}
#endif

#ifdef __WATCOMC__
long fast_imul16(int a, int b);
#pragma aux fast_imul16 = \
    "imul dx" \
    parm [ax] [dx] \
    value [dx ax];
#else
static long fast_imul16(int a, int b) { return (long)a * (long)b; }
#endif

static long fast_mul16_signed(int a, unsigned int b)
{
    if (a < 0) return -(long)fast_mul16((unsigned int)(-a), b);
    return (long)fast_mul16((unsigned int)a, b);
}

static int fds_wave_dirty = 1;
#define FDS_SEG 16

static unsigned char w256[256];
static int vtab[64];
static int vtab_key = -1;

static unsigned int a_ph, a_inc, a_rcl, a_n, a_mix;
static int a_y, a_dc;
static unsigned char far *a_out;

static const int mod_bias[8] = { 0, 1, 2, 4, 0 , -4, -2, -1 };

static unsigned int master_table[4];
static int master_table_built = 0;

static void build_master_table(void)
{
    double master_vol = 2.4 * 1223.0;
    double max_out = 32.0 * 63.0;
    int i;
    static const double divs[4] = { 2.0, 3.0, 4.0, 5.0 };
    if (master_table_built) return;
    for (i = 0; i < 4; i++)
        master_table[i] = (unsigned int)((master_vol / max_out) * 256.0 * 2.0 / divs[i]);
    master_table_built = 1;
}

void fds_init(FdsApu *f, unsigned long clock, unsigned long sample_rate)
{
    double leak;
    memset(f, 0, sizeof(*f));
    build_master_table();
    f->clock = clock;
    f->sample_rate = sample_rate;
    f->clock_step = (unsigned long)(((double)clock / (double)sample_rate) * 1048576.0 + 0.5);
    f->prev_out = 128;
    f->click_delta = (int)((16UL * 44100UL) / sample_rate);
    if (f->click_delta < 16) f->click_delta = 16;
    leak = exp(-2.0 * 3.14159265 * 2000.0 / (double)sample_rate);
    f->rc_k = (long)(leak * 256.0);
    f->rc_l = 256L - f->rc_k;
    vtab_key = -1;
    fds_wave_dirty = 1;
    fds_write_reg(f, 0x4023, 0x00);
    fds_write_reg(f, 0x4023, 0x83);
    fds_write_reg(f, 0x4080, 0x80);
    fds_write_reg(f, 0x408A, 0xFF);
    fds_write_reg(f, 0x4082, 0x00);
    fds_write_reg(f, 0x4083, 0x80);
    fds_write_reg(f, 0x4084, 0x80);
    fds_write_reg(f, 0x4085, 0x00);
    fds_write_reg(f, 0x4086, 0x00);
    fds_write_reg(f, 0x4087, 0x80);
    fds_write_reg(f, 0x4089, 0x00);
}

void fds_write_reg(FdsApu *f, unsigned int addr, unsigned char data)
{
    if (addr == 0x4023) {
        f->master_io = (unsigned char)((data & 2) ? 1 : 0);
        return;
    }
    if (!f->master_io) return;
    if (addr < 0x4040 || addr > 0x408A) return;
    if (addr < 0x4080) {
        if (f->wav_write)
        {
            f->wave[addr - 0x4040] = (unsigned char)(data & 0x3F);
            fds_wave_dirty = 1;
        }
        return;
    }
    switch (addr) {
        case 0x4080:
            f->env_disable[1] = (unsigned char)((data & 0x80) ? 1 : 0);
            f->env_mode[1] = (unsigned char)((data & 0x40) ? 1 : 0);
            f->env_timer[1] = 0;
            f->env_speed[1] = (unsigned char)(data & 0x3F);
            if (f->env_disable[1]) f->env_out[1] = f->env_speed[1];
            break;
        case 0x4082:
            f->freq_wav = (f->freq_wav & 0x0F00) | data;
            break;
        case 0x4083:
            f->freq_wav = (f->freq_wav & 0x00FF) | ((unsigned int)(data & 0x0F) << 8);
            f->wav_halt = (unsigned char)((data & 0x80) ? 1 : 0);
            f->env_halt = (unsigned char)((data & 0x40) ? 1 : 0);
            if (f->wav_halt) f->phase_wav = 0;
            if (f->env_halt) { f->env_timer[0] = 0; f->env_timer[1] = 0; }
            break;
        case 0x4084:
            f->env_disable[0] = (unsigned char)((data & 0x80) ? 1 : 0);
            f->env_mode[0] = (unsigned char)((data & 0x40) ? 1 : 0);
            f->env_timer[0] = 0;
            f->env_speed[0] = (unsigned char)(data & 0x3F);
            if (f->env_disable[0]) f->env_out[0] = f->env_speed[0];
            break;
        case 0x4085:
            f->mod_pos = data & 0x7F;
            break;
        case 0x4086:
            f->freq_mod = (f->freq_mod & 0x0F00) | data;
            break;
        case 0x4087:
            f->freq_mod = (f->freq_mod & 0x00FF) | ((unsigned int)(data & 0x0F) << 8);
            f->mod_halt = (unsigned char)((data & 0x80) ? 1 : 0);
            if (f->mod_halt) f->phase_mod &= 0x3F0000UL;
            break;
        case 0x4088:
            if (f->mod_halt) {
                f->mod_wave[(f->phase_mod >> 16) & 0x3F] = (unsigned char)(data & 0x07);
                f->phase_mod = (f->phase_mod + 0x10000UL) & 0x3FFFFFUL;
                f->mod_wave[(f->phase_mod >> 16) & 0x3F] = (unsigned char)(data & 0x07);
                f->phase_mod = (f->phase_mod + 0x10000UL) & 0x3FFFFFUL;
                f->mod_write_pos = (unsigned int)(f->phase_mod >> 16);
            }
            break;
        case 0x4089:
            f->wav_write = (unsigned char)((data & 0x80) ? 1 : 0);
            f->master_vol = (unsigned char)(data & 0x03);
            break;
        case 0x408A:
            f->master_env_speed = data;
            f->env_timer[0] = 0;
            f->env_timer[1] = 0;
            break;
        default:
            break;
    }
}

static void fds_seg_set(void)
{
    _asm {
        push bx
        push cx
        push dx
        push es
        push si
        push di
        mov cx, a_ph
        mov si, a_y
        les di, a_out
        xor bx, bx
    fset:
        add cx, a_inc
        mov bl, ch
        mov bl, w256[bx]
        mov ax, vtab[bx]
        sub ax, si
        imul a_rcl
        mov al, ah
        mov ah, dl
        add si, ax
        mov ax, si
        sub ax, a_dc
        mov al, ah
        xor al, 0x80
        stosb
        dec a_n
        jnz fset
        mov a_ph, cx
        mov a_y, si
        pop di
        pop si
        pop es
        pop dx
        pop cx
        pop bx
    }
}

static void fds_seg_mix(void)
{
    _asm {
        push bx
        push cx
        push dx
        push es
        push si
        push di
        mov cx, a_ph
        mov si, a_y
        les di, a_out
        xor bx, bx
    fmix:
        add cx, a_inc
        mov bl, ch
        mov bl, w256[bx]
        mov ax, vtab[bx]
        sub ax, si
        imul a_rcl
        mov al, ah
        mov ah, dl
        add si, ax
        mov ax, si
        sub ax, a_dc
        mov al, ah
        cbw
        mov dl, es:[di]
        xor dh, dh
        add ax, dx
        test ah, ah
        jz fok
        sar ah, 1
        not ah
        mov al, ah
    fok:
        stosb
        dec a_n
        jnz fmix
        mov a_ph, cx
        mov a_y, si
        pop di
        pop si
        pop es
        pop dx
        pop cx
        pop bx
    }
}

void fds_run(FdsApu *f, unsigned char far *out, unsigned long n, int mix_mode)
{
    unsigned int cps = (unsigned int)((f->clock_step + 2048UL) >> 12);
    unsigned int master = master_table[f->master_vol];
    unsigned int k;
    if (fds_wave_dirty) {
        for (k = 0; k < 256; k++) w256[k] = (unsigned char)(f->wave[k >> 2] << 1);
        fds_wave_dirty = 0;
    }
    a_ph = (unsigned int)f->phase_wav;
    a_y = (int)f->rc_accum;
    a_dc = (int)f->dc_avg;
    a_rcl = (unsigned int)f->rc_l;
    a_mix = (unsigned int)mix_mode;
    a_out = out;
    while (n) {
        unsigned int seg = n > FDS_SEG ? FDS_SEG : (unsigned int)n;
        unsigned long segq = (unsigned long)cps * seg;
        int vol, key;
        n -= seg;
        if (!f->env_halt && !f->wav_halt && f->master_env_speed != 0) {
            int e;
            for (e = 0; e < 2; e++) {
                unsigned long per;
                if (f->env_disable[e]) continue;
                per = ((unsigned long)((unsigned int)(f->env_speed[e] + 1) * (unsigned int)f->master_env_speed) << 3) << 8;
                f->env_timer[e] += segq;
                while (f->env_timer[e] >= per) {
                    f->env_timer[e] -= per;
                    if (f->env_mode[e]) { if (f->env_out[e] < 32) f->env_out[e]++; }
                    else { if (f->env_out[e] > 0) f->env_out[e]--; }
                }
            }
        }
        if (!f->mod_halt && f->freq_mod) {
            unsigned int p0 = (unsigned int)(f->phase_mod >> 16);
            unsigned int p1;
            f->phase_mod += ((segq >> 4) * f->freq_mod) >> 4;
            p1 = (unsigned int)(f->phase_mod >> 16);
            f->phase_mod &= 0x3FFFFFUL;
            while (p0 != p1) {
                int wv = f->mod_wave[p0 & 0x3F];
                if (wv == 4) f->mod_pos = 0;
                else f->mod_pos = (f->mod_pos + mod_bias[wv & 7]) & 0x7F;
                p0++;
            }
        }
        a_inc = 0;
        if (!f->wav_halt) {
            long fe = (long)f->freq_wav;
            if (f->env_out[0] != 0) {
                int pos = (f->mod_pos < 64) ? f->mod_pos : (f->mod_pos - 128);
                int temp = (pos * f->env_out[0]) >> 4;
                if (temp >= 192) temp -= 256;
                else if (temp < -64) temp += 256;
                fe += fast_mul16_signed(temp, f->freq_wav) >> 6;
            }
            if (fe < 0) fe = 0;
            else if (fe > 0xFFFFL) fe = 0xFFFFL;
            {
                unsigned long inc = (fast_mul16(cps, (unsigned int)fe)) >> 14;
                a_inc = inc > 0x7FFFUL ? 0x7FFF : (unsigned int)inc;
            }
        }
        vol = f->env_out[1] > 32 ? 32 : f->env_out[1];
        key = f->wav_write ? -2 : vol;
        if (key != vtab_key) {
            unsigned int step = f->wav_write ? 0 : (unsigned int)((fast_mul16((unsigned int)vol, master)) >> 5);
            unsigned int acc = 0;
            for (k = 0; k < 64; k++) { vtab[k] = (int)acc; acc += step; }
            vtab_key = key;
        }
        a_n = seg;
        if (a_mix) fds_seg_mix(); else fds_seg_set();
        a_out += seg;
        a_dc += (a_y - a_dc) >> 3;
    }
    f->phase_wav = a_ph;
    f->rc_accum = a_y;
    f->dc_avg = a_dc;
}
