#include <string.h>
#include "wsapu.h"
#include "dosmem.h"
#include "fastmath.h"
#define WS_NOISE_MODES 7

#define PREFIX_AT_FAST(prefix, period, total, k) \
    ((unsigned int)(k) >= (unsigned int)(period) ? (unsigned int)(total) : (prefix)[(unsigned int)(k)])

static const unsigned int ws_tap_bit[8]  = { 14, 10, 13, 4, 8, 6, 9, 11 };
static const unsigned int ws_noise_period[8] = { 32767, 1953, 254, 217, 73, 63, 42, 28 };

static DosBuffer noise_buf[8], prefix_buf[8];
static unsigned char far *noise_table[8];
static unsigned int far *prefix_table[8];
static unsigned int total_ones[8];
static int ws_noise_tables_built = 0;
static int ws_block(WsApu *a, unsigned int blk, int create);
static int wkey_ok[4];

static void build_ws_noise_tables(void)
{
    int mode;
    if (ws_noise_tables_built) return;
    for (mode = 0; mode < 8; mode++) {
        unsigned short lfsr;
        unsigned long i;
        unsigned int running;
        unsigned int period = ws_noise_period[mode];
        unsigned int tapbit = ws_tap_bit[mode];
        if (!dosmem_alloc_simple(&noise_buf[mode], period)) {
            int k;
            for (k = 0; k < mode; k++) {
                dosmem_free(&noise_buf[k]);
                dosmem_free(&prefix_buf[k]);
            }
            return;
        }
        if (!dosmem_alloc_simple(&prefix_buf[mode], period * 2U)) {
            int k;
            dosmem_free(&noise_buf[mode]);
            for (k = 0; k < mode; k++) {
                dosmem_free(&noise_buf[k]);
                dosmem_free(&prefix_buf[k]);
            }
            return;
        }
        noise_table[mode] = noise_buf[mode].ptr;
        prefix_table[mode] = (unsigned int far *)prefix_buf[mode].ptr;
        lfsr = 0x7FFF;
        running = 0;
        for (i = 0; i < period; i++) {
            unsigned int bit0 = lfsr & 1;
            unsigned int tap = (lfsr >> tapbit) & 1;
            unsigned int newbit = (unsigned int)(!(bit0 ^ tap));
            prefix_table[mode][i] = running;
            noise_table[mode][i] = (unsigned char)bit0;
            running = (unsigned int)(running + bit0);
            lfsr = (unsigned short)(((lfsr << 1) | newbit) & 0x7FFF);
        }
        total_ones[mode] = running;
    }
    ws_noise_tables_built = 1;
}

void wsapu_free_noise_tables(void)
{
    int mode;
    if (!ws_noise_tables_built) return;
    for (mode = 0; mode < 8; mode++) {
        dosmem_free(&noise_buf[mode]);
        dosmem_free(&prefix_buf[mode]);
    }
    ws_noise_tables_built = 0;
}

static unsigned int prefix_at(const unsigned int far *prefix, unsigned int period,
                               unsigned int total, unsigned int k)
{
    return (k >= period) ? total : prefix[k];
}

#define FIXED_SHIFT 8
#define FIXED_ONE (1UL << FIXED_SHIFT)
#define FULL_CYCLE 8192U

void wsapu_init(WsApu *a, unsigned long clock, unsigned long sample_rate)
{
    memset(a, 0, sizeof(*a));
    a->vt_dirty = 1;
    {
        int i;
        for (i = 0; i < 8; i++) a->blk_addr[i] = 0xFFFFU;
    }
    build_ws_noise_tables();
    a->clock = clock;
    a->sample_rate = sample_rate;
    a->ticks_cpu_precalc = (unsigned long)((clock << FIXED_SHIFT) / sample_rate);
    a->prev_out = 128;
    a->click_delta = (int)((16UL * 44100UL) / sample_rate);
    if (a->click_delta < 16) a->click_delta = 16;
    wkey_ok[0] = wkey_ok[1] = wkey_ok[2] = wkey_ok[3] = 0;
}

void wsapu_write_port(WsApu *a, unsigned char port, unsigned char data)
{
    a->vt_dirty = 1;
    if (port >= 0x80 && port <= 0x87) {
        unsigned char ch = (unsigned char)((port - 0x80) >> 1);
        if (port & 1)
            a->freq[ch] = (unsigned short)((a->freq[ch] & 0x00FF) | ((data & 0x07) << 8));
        else
            a->freq[ch] = (unsigned short)((a->freq[ch] & 0xFF00) | data);
        return;
    }
    if (port >= 0x88 && port <= 0x8B) {
        unsigned char ch = (unsigned char)(port - 0x88);
        a->vol_l[ch] = (unsigned char)(data >> 4);
        a->vol_r[ch] = (unsigned char)(data & 0x0F);
        return;
    }
    switch (port) {
        case 0x8C: a->sweep_value = (signed char)data; break;
        case 0x8D:
            a->sweep_ticks = (unsigned char)(data & 0x1F);
            a->sweep_counter = 0;
            break;
        case 0x8E:
            if (data & 0x08) a->noise_idx = 0;
            a->noise_ctrl = data;
            break;
        case 0x8F:
            a->wave_base = (unsigned int)data << 6;
            {
                int b = ws_block(a, a->wave_base, 0);
                if (b >= 0) memcpy(a->wave_ram, a->blk_data[b], 64);
            }
            break;
        case 0x90: a->ch_enable = data; break;
        default: break;
    }
}

static int ws_block(WsApu *a, unsigned int blk, int create)
{
    int i;
    for (i = 0; i < 8; i++) if (a->blk_addr[i] == blk) return i;
    if (!create) return -1;
    i = a->blk_next;
    a->blk_next = (unsigned char)((i + 1) & 7);
    a->blk_addr[i] = blk;
    memset(a->blk_data[i], 0, 64);
    return i;
}

void wsapu_write_mem(WsApu *a, unsigned int addr, unsigned char data)
{
    int b = ws_block(a, addr & 0xFFC0U, 1);
    a->blk_data[b][addr & 63] = data;
    a->vt_dirty = 1;
    if (addr >= a->wave_base && addr < a->wave_base + 64U)
        a->wave_ram[addr - a->wave_base] = data;
}

static unsigned char wt[4][256];
static unsigned char wkey[4][18];

static unsigned int w_ph0, w_ph1, w_ph2, w_ph3, w_inc0, w_inc1, w_inc2, w_inc3;
static unsigned int w_nacc, w_nstep, w_nidx, w_nper, w_nseg, w_n;
static unsigned char w_nact, w_namp;
static int w_prev, w_cd;
static unsigned int w_tmp;
static unsigned char far *w_out;

static unsigned int ws_inc(unsigned long base, unsigned int div)
{
    if (div == 0 || (unsigned int)(base >> 16) >= div) return 0;
    return fast_div16(base, div);
}

static void ws_table(WsApu *a, int ch, int on, unsigned int vol)
{
    unsigned char key[18];
    int k;
    for (k = 0; k < 16; k++) key[k] = a->wave_ram[(ch << 4) + k];
    key[16] = (unsigned char)vol;
    key[17] = (unsigned char)on;
    if (wkey_ok[ch] && memcmp(key, wkey[ch], 18) == 0) return;
    memcpy(wkey[ch], key, 18);
    wkey_ok[ch] = 1;
    if (!on || vol == 0) { memset(wt[ch], 0, 256); return; }
    for (k = 0; k < 32; k++) {
        int n0 = (k & 1) ? (key[k >> 1] >> 4) : (key[k >> 1] & 15);
        int k1 = (k + 1) & 31;
        int n1 = (k1 & 1) ? (key[k1 >> 1] >> 4) : (key[k1 >> 1] & 15);
        int v0 = n0 * (int)vol * 13, d = (n1 - n0) * (int)vol * 13, j;
        unsigned char *t = wt[ch] + (k << 3);
        for (j = 0; j < 8; j++) t[j] = (unsigned char)((v0 + ((d * j) >> 3) + 16) >> 5);
    }
}

static void ws_prep(WsApu *a)
{
    unsigned long base = ((unsigned long)a->clock * 256UL) / a->sample_rate;
    unsigned char en = a->ch_enable;
    int noise = ws_noise_tables_built && (a->noise_ctrl & 0x80) && (en & 0x08) && (en & 0x80);
    int ch;
    for (ch = 0; ch < 4; ch++) {
        unsigned int vol = (unsigned int)((a->vol_l[ch] + a->vol_r[ch]) >> 1);
        int on = (en >> ch) & 1;
        if (ch == 3 && noise) on = 0;
        ws_table(a, ch, on, vol);
        {
            unsigned int inc = on ? ws_inc(base << 3, 2048U - a->freq[ch]) : 0;
            if (ch == 0) w_inc0 = inc; else if (ch == 1) w_inc1 = inc;
            else if (ch == 2) w_inc2 = inc; else w_inc3 = inc;
        }
    }
    w_nact = 0;
    if (noise) {
        unsigned int mode = a->noise_ctrl & 7;
        unsigned int vol = (unsigned int)((a->vol_l[3] + a->vol_r[3]) >> 1);
        const unsigned char far *t = noise_table[mode];
        unsigned long st = a->ticks_cpu_precalc / (2048U - a->freq[3]);
        w_nper = ws_noise_period[mode];
        if (st > (unsigned long)(w_nper - 1) << 8) st = (unsigned long)(w_nper - 1) << 8;
        w_nstep = (unsigned int)st;
        if (w_nidx >= w_nper) w_nidx = 0;
        w_namp = (unsigned char)((15U * vol * 13U + 16U) >> 5);
        w_nseg = FP_SEG(t) + (FP_OFF(t) >> 4);
        if ((FP_OFF(t) & 15) == 0 && w_namp) w_nact = 1;
    }
}

static void ws_mix_asm(void)
{
    _asm {
        push bx
        push cx
        push dx
        push es
        push si
        push di
        mov cx, w_n
        les di, w_out
        mov si, w_ph0
        mov dx, w_ph1
        push bp
        mov ax, w_ph2
        mov bp, ax
    wlp:
        xor ax, ax
        add si, w_inc0
        mov bx, si
        mov bl, bh
        xor bh, bh
        mov al, wt[bx]
        add dx, w_inc1
        mov bx, dx
        mov bl, bh
        xor bh, bh
        add al, wt+256[bx]
        adc ah, 0
        add bp, w_inc2
        mov bx, bp
        mov bl, bh
        xor bh, bh
        add al, wt+512[bx]
        adc ah, 0
        mov bx, w_ph3
        add bx, w_inc3
        mov w_ph3, bx
        mov bl, bh
        xor bh, bh
        add al, wt+768[bx]
        adc ah, 0
        cmp w_nact, 0
        je  wnoz
        mov w_tmp, ax
        mov bx, w_nacc
        add bx, w_nstep
        mov byte ptr w_nacc, bl
        mov bl, bh
        xor bh, bh
        add bx, w_nidx
        cmp bx, w_nper
        jb  wnok
        sub bx, w_nper
    wnok:
        mov w_nidx, bx
        push ds
        mov ds, w_nseg
        cmp byte ptr [bx], 0
        pop ds
        mov ax, w_tmp
        jne wnoz
        add al, w_namp
        adc ah, 0
    wnoz:
        add ax, 128
        cmp ax, 255
        jbe wcl
        mov ax, 255
    wcl:
        mov bx, ax
        sub bx, w_prev
        cmp bx, w_cd
        jle wc1
        mov ax, w_prev
        add ax, w_cd
        xor bx, bx
    wc1:
        neg bx
        cmp bx, w_cd
        jle wc2
        mov ax, w_prev
        sub ax, w_cd
    wc2:
        mov w_prev, ax
        stosb
        dec cx
        jz  wdone
        jmp wlp
    wdone:
        mov w_ph2, bp
        pop bp
        mov w_ph0, si
        mov w_ph1, dx
        pop di
        pop si
        pop es
        pop dx
        pop cx
        pop bx
    }
}

void wsapu_run(WsApu *a, unsigned char far *out, unsigned long count)
{
    int sweep_on = (a->ch_enable & 0x40) && (a->ch_enable & 0x04);
    long reload = (long)(((a->sweep_ticks + 1UL) * a->sample_rate * 256UL) / 375UL);
    int ch;
    w_ph0 = a->phase[0]; w_ph1 = a->phase[1]; w_ph2 = a->phase[2]; w_ph3 = a->phase[3];
    w_nidx = a->noise_idx;
    w_nacc = (unsigned int)(a->noise_cycle_accum & 0xFF);
    w_prev = a->prev_out;
    w_cd = a->click_delta;
    if (reload < 256) reload = 256;
    ws_prep(a);
    while (count) {
        unsigned int n = count > 60000UL ? 60000U : (unsigned int)count;
        if (sweep_on) {
            if (a->sweep_counter <= 0) {
                while (a->sweep_counter <= 0) a->sweep_counter += reload;
                a->freq[2] = (unsigned short)((a->freq[2] + a->sweep_value) & 0x07FF);
                w_inc2 = ws_inc((((unsigned long)a->clock * 256UL) / a->sample_rate) << 3, 2048U - a->freq[2]);
            }
            {
                unsigned long left = (unsigned long)((a->sweep_counter + 255) >> 8);
                if (left < n) n = (unsigned int)left;
            }
        }
        count -= n;
        w_out = out; w_n = n;
        ws_mix_asm();
        out += n;
        if (sweep_on) a->sweep_counter -= (long)n << 8;
    }
    a->phase[0] = w_ph0; a->phase[1] = w_ph1; a->phase[2] = w_ph2; a->phase[3] = w_ph3;
    a->noise_idx = w_nidx;
    a->noise_cycle_accum = w_nacc;
    a->prev_out = (unsigned char)w_prev;
}
