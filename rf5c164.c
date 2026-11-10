#include <stdio.h>
#include <string.h>
#include <dos.h>
#include "rf5c164.h"
#include "emu8000.h"
#include "awetab.h"

typedef struct {
    unsigned char env, pan, start, on;
    unsigned fd, ls;
    unsigned idx, frac;
    unsigned stepi, stepf;
    int dirty;
    unsigned tabvol;
    unsigned char awe;
} RfChan;

static RfChan s_ch[8];
static int s_boost = 0;
static int s_half = 0;
static unsigned char s_enable = 0, s_cbank = 0, s_wbank = 0;
static unsigned s_ram_seg = 0;
static unsigned s_bank_seg = 0;
static unsigned long s_bank_size = 0;
static long s_bank_last = -1;
static void bank_release(void);
static unsigned long s_ratio = 0;
static unsigned long s_clock = 12500000UL;
static unsigned long s_wptr = 0;
static void awe_stop_all(void);
static void awe_release(void);
static int s_awe_on = 0;
static unsigned char s_rawe[8];

static unsigned char s_tabmem[9 * 256];
static unsigned s_tab0;
static int s_mix[512];

static unsigned a_n, a_idx, a_frac, a_stepi, a_stepf, a_ls, a_tab, a_mix, a_rseg;
static unsigned char a_stop;
static unsigned char far *a_out;

static void chan_step(RfChan *c)
{
    unsigned long st = (((unsigned long)c->fd * (s_ratio >> 4)) >> 7) +
                       (((unsigned long)c->fd * (s_ratio & 15UL)) >> 11);
    c->stepi = (unsigned)(st >> 16);
    c->stepf = (unsigned)(st & 0xFFFFUL);
}

void rf5c_set_rate(unsigned long sample_rate)
{
    int k;
    s_ratio = ((s_clock / 3UL) << 9) / sample_rate;
    for (k = 0; k < 8; k++) chan_step(&s_ch[k]);
}

int rf5c_init(unsigned long clock, unsigned long sample_rate)
{
    int k;
    awe_stop_all();
    memset(s_ch, 0, sizeof(s_ch));
    s_enable = 0; s_cbank = 0; s_wbank = 0; s_wptr = 0; s_boost = 0; s_half = 0;
    bank_release();
    s_clock = clock ? clock : 12500000UL;
    s_tab0 = ((unsigned)s_tabmem + 255U) & 0xFF00U;
    for (k = 0; k < 8; k++) { s_ch[k].dirty = 1; s_ch[k].tabvol = 0xFFFFU; }
    if (!s_ram_seg) {
        unsigned seg;
        if (_dos_allocmem(0x1000, &seg) != 0) return 0;
        s_ram_seg = seg;
    }
    {
        unsigned char far *p = (unsigned char far *)MK_FP(s_ram_seg, 0);
        _fmemset(p, 0xFF, 0x8000U);
        _fmemset(p + 0x8000U, 0xFF, 0x8000U);
    }
    rf5c_set_rate(sample_rate);
    return 1;
}

void rf5c_free(void)
{
    awe_stop_all();
    awe_release();
    bank_release();
    if (s_ram_seg) { _dos_freemem(s_ram_seg); s_ram_seg = 0; }
}

void rf5c_write(unsigned reg, unsigned data)
{
    RfChan *c = &s_ch[s_cbank];
    int k;
    switch (reg) {
        case 0x00: c->env = (unsigned char)data; c->dirty = 1; break;
        case 0x01: c->pan = (unsigned char)data; c->dirty = 1; break;
        case 0x02: c->fd = (c->fd & 0xFF00U) | data; chan_step(c); break;
        case 0x03: c->fd = (c->fd & 0x00FFU) | (data << 8); chan_step(c); break;
        case 0x04: c->ls = (c->ls & 0xFF00U) | data; break;
        case 0x05: c->ls = (c->ls & 0x00FFU) | (data << 8); break;
        case 0x06:
            c->start = (unsigned char)data;
            if (!c->on) { c->idx = (unsigned)data << 8; c->frac = 0; }
            break;
        case 0x07:
            s_enable = (unsigned char)((data >> 7) & 1);
            if (data & 0x40) s_cbank = (unsigned char)(data & 7);
            else s_wbank = (unsigned char)(data & 15);
            break;
        case 0x08:
            for (k = 0; k < 8; k++) {
                unsigned char was = s_ch[k].on;
                s_ch[k].on = (unsigned char)((~data >> k) & 1);
                if (!s_ch[k].on) { s_ch[k].idx = (unsigned)s_ch[k].start << 8; s_ch[k].frac = 0; s_ch[k].awe = 0; }
                else if (!was && s_awe_on) { if (s_rawe[k]) { s_ch[k].awe = 1; } }
            }
            break;
    }
}

void rf5c_mem(unsigned addr, unsigned data)
{
    unsigned char far *p;
    if (!s_ram_seg) return;
    p = (unsigned char far *)MK_FP(s_ram_seg, 0);
    p[((unsigned)s_wbank << 12) | (addr & 0x0FFFU)] = (unsigned char)data;
}

void rf5c_ram_begin(unsigned addr)
{
    s_wptr = (unsigned long)(((unsigned)s_wbank << 12) | addr);
}

void rf5c_ram_data(const unsigned char *src, unsigned n)
{
    unsigned char far *p;
    if (!s_ram_seg || s_wptr >= 0x10000UL) return;
    if ((unsigned long)n > 0x10000UL - s_wptr) n = (unsigned)(0x10000UL - s_wptr);
    p = (unsigned char far *)MK_FP(s_ram_seg, 0);
    _fmemcpy(p + (unsigned)s_wptr, src, n);
    s_wptr += n;
}

enum { BK_NONE, BK_CONV, BK_XMS, BK_FILE, BK_TIER };

static unsigned long s_t1 = 0, s_t2 = 0;
static int s_bk_mode = BK_NONE;
static FILE *s_bk_file = 0;
#define BK_SEGS 32
static struct { long foff; unsigned long start, size; } s_bk_seg[BK_SEGS];
static int s_bk_nseg = 0;
static unsigned char s_bk_tmp[1024];

#pragma pack(1)
typedef struct {
    unsigned long len;
    unsigned sh;
    unsigned long so;
    unsigned dh;
    unsigned long doff;
} XmsMove;
#pragma pack()
static void far *s_xms = 0;
static unsigned s_xms_h = 0;
static unsigned long s_xms_bytes = 0;
static XmsMove s_mv;

unsigned char xms_present(void);
#pragma aux xms_present = "mov ax, 4300h" "int 2Fh" value [al] modify [ax];
void far *xms_entry(void);
#pragma aux xms_entry = "mov ax, 4310h" "int 2Fh" value [es bx] modify [ax es bx];

unsigned long xms_call(unsigned a, unsigned d, unsigned si_, void far *entry);
#pragma aux xms_call = \
    "push es" "push bx" "mov bx, sp" "call dword ptr ss:[bx]" "add sp, 4" \
    parm [ax] [dx] [si] [es bx] value [dx ax] modify [ax bx cx dx es];

static int xms_alloc(unsigned long need)
{
    unsigned kb, largest;
    unsigned long r;
    if (!s_xms) {
        if ((xms_present() & 0xFF) != 0x80) return 0;
        s_xms = xms_entry();
        if (!s_xms) return 0;
    }
    largest = (unsigned)xms_call(0x0800, 0, 0, s_xms);
    kb = (unsigned)((need + 1023UL) >> 10);
    if (kb < 640) kb = 640;
    if (kb > largest) kb = largest;
    if ((unsigned long)kb * 1024UL < need) return 0;
    r = xms_call(0x0900, kb, 0, s_xms);
    if ((unsigned)r != 1) return 0;
    s_xms_h = (unsigned)(r >> 16);
    s_xms_bytes = (unsigned long)kb * 1024UL;
    return 1;
}

static unsigned long xms_alloc_upto(unsigned long need)
{
    unsigned kb, largest;
    unsigned long r;
    if (!s_xms) {
        if ((xms_present() & 0xFF) != 0x80) return 0;
        s_xms = xms_entry();
        if (!s_xms) return 0;
    }
    largest = (unsigned)xms_call(0x0800, 0, 0, s_xms);
    kb = (unsigned)((need + 1023UL) >> 10);
    if (kb > largest) kb = largest;
    if (kb < 16) return 0;
    r = xms_call(0x0900, kb, 0, s_xms);
    if ((unsigned)r != 1) return 0;
    s_xms_h = (unsigned)(r >> 16);
    s_xms_bytes = (unsigned long)kb * 1024UL;
    return s_xms_bytes;
}

static void xms_free(void)
{
    if (s_xms_h) { xms_call(0x0A00, s_xms_h, 0, s_xms); s_xms_h = 0; }
    s_xms_bytes = 0;
}

static int xms_move(int to_xms, unsigned long xoff, void far *p, unsigned long n)
{
    unsigned long fp = ((unsigned long)FP_SEG(p) << 16) | FP_OFF(p);
    s_mv.len = n;
    if (to_xms) { s_mv.sh = 0; s_mv.so = fp; s_mv.dh = s_xms_h; s_mv.doff = xoff; }
    else { s_mv.sh = s_xms_h; s_mv.so = xoff; s_mv.dh = 0; s_mv.doff = fp; }
    return (unsigned)xms_call(0x0B00, 0, (unsigned)&s_mv, s_xms) == 1;
}

static void bank_release(void)
{
    if (s_bank_seg) { _dos_freemem(s_bank_seg); s_bank_seg = 0; }
    xms_free();
    s_bank_size = 0;
    s_bk_mode = BK_NONE;
    s_bk_nseg = 0;
    s_bank_last = -1;
    s_t1 = s_t2 = 0;
}

static int conv_to_xms(unsigned long newtotal)
{
    unsigned long pos = 0;
    if (!xms_alloc(newtotal + 2)) return 0;
    while (pos < s_bank_size) {
        unsigned long k = s_bank_size - pos;
        if (k > 1024UL) k = 1024UL;
        _fmemcpy(s_bk_tmp, MK_FP(s_bank_seg + (unsigned)(pos >> 4), (unsigned)(pos & 15UL)), (unsigned)k);
        if (!xms_move(1, pos, (void far *)s_bk_tmp, (k + 1) & ~1UL)) { xms_free(); return 0; }
        pos += k;
    }
    _dos_freemem(s_bank_seg); s_bank_seg = 0;
    return 1;
}

static int tier_setup(unsigned long total)
{
    unsigned seg, maxp = 0;
    unsigned long c = 0, x;
    if (_dos_allocmem(0xFFFFU, &seg) != 0) maxp = seg;
    else { _dos_freemem(seg); maxp = 0xFFFFU; }
    if (maxp > 4096U + 1024U) {
        maxp -= 4096U;
        c = (unsigned long)maxp << 4;
        if (c > total) { c = (total + 15UL) & ~15UL; maxp = (unsigned)(c >> 4); }
        if (_dos_allocmem(maxp, &seg) == 0) s_bank_seg = seg; else c = 0;
    }
    s_t1 = c;
    x = (total > c) ? xms_alloc_upto(total - c + 2) : 0;
    if (x > total - c) x = total - c;
    s_t2 = c + (x & ~1UL);
    return 1;
}

int rf5c_bank_load(FILE *f, long offset, unsigned long size)
{
    unsigned long total = s_bank_size + size, pos;
    unsigned paras = (unsigned)((total + 15UL) >> 4), seg, maxp;
    int big = (total > 0x9FFF0UL);
    if (!f || size == 0 || total > 0xFFFFFFUL) return 0;
    if (offset <= s_bank_last) return 1;
    s_bank_last = offset;
    s_bk_file = f;
    if (s_bk_nseg < BK_SEGS) {
        s_bk_seg[s_bk_nseg].foff = offset;
        s_bk_seg[s_bk_nseg].start = s_bank_size;
        s_bk_seg[s_bk_nseg].size = size;
        s_bk_nseg++;
    } else {
        return 0;
    }
    if (s_bk_mode == BK_NONE) {
        if (!big && _dos_allocmem(paras, &seg) == 0) { s_bank_seg = seg; s_bk_mode = BK_CONV; }
        else if (xms_alloc(total + 2)) s_bk_mode = BK_XMS;
        else if (s_bank_size == 0 && tier_setup(total)) s_bk_mode = BK_TIER;
        else s_bk_mode = BK_FILE;
    } else if (s_bk_mode == BK_CONV) {
        if (big || _dos_setblock(paras, s_bank_seg, &maxp) != 0) {
            if (conv_to_xms(total)) s_bk_mode = BK_XMS;
            else { _dos_freemem(s_bank_seg); s_bank_seg = 0; s_bk_mode = BK_FILE; }
        }
    } else if (s_bk_mode == BK_XMS) {
        if (total + 2 > s_xms_bytes) { xms_free(); s_bk_mode = BK_FILE; }
    }
    if (s_bk_mode == BK_TIER) {
        if (fseek(f, offset, SEEK_SET) != 0) return 0;
        pos = s_bank_size;
        while (pos < total && pos < s_t2) {
            unsigned long lim = (pos < s_t1) ? s_t1 : s_t2;
            unsigned n = (lim - pos > sizeof(s_bk_tmp)) ? (unsigned)sizeof(s_bk_tmp) : (unsigned)(lim - pos);
            unsigned got = (unsigned)fread(s_bk_tmp, 1, n, f);
            if (pos < s_t1)
                _fmemcpy(MK_FP(s_bank_seg + (unsigned)(pos >> 4), (unsigned)(pos & 15UL)), s_bk_tmp, got);
            else
                xms_move(1, pos - s_t1, (void far *)s_bk_tmp, ((unsigned long)got + 1) & ~1UL);
            pos += got;
            if (got < n) break;
        }
        s_bank_size = total;
        return 1;
    }
    if (s_bk_mode == BK_CONV || s_bk_mode == BK_XMS) {
        if (fseek(f, offset, SEEK_SET) != 0) return 0;
        pos = s_bank_size;
        while (pos < total) {
            unsigned n = (total - pos > sizeof(s_bk_tmp)) ? (unsigned)sizeof(s_bk_tmp) : (unsigned)(total - pos);
            unsigned got = (unsigned)fread(s_bk_tmp, 1, n, f);
            if (s_bk_mode == BK_CONV)
                _fmemcpy(MK_FP(s_bank_seg + (unsigned)(pos >> 4), (unsigned)(pos & 15UL)), s_bk_tmp, got);
            else
                xms_move(1, pos, (void far *)s_bk_tmp, ((unsigned long)got + 1) & ~1UL);
            pos += got;
            if (got < n) break;
        }
        total = pos;
    }
    s_bank_size = total;
    return 1;
}

static void copy_conv(unsigned char far *ram, unsigned long src, unsigned long d, unsigned long n)
{
    while (n > 0) {
        unsigned k = (n > 0x8000UL) ? 0x8000U : (unsigned)n;
        _fmemcpy(ram + (unsigned)d, MK_FP(s_bank_seg + (unsigned)(src >> 4), (unsigned)(src & 15UL)), k);
        src += k; d += k; n -= k;
    }
}

static void copy_xms(unsigned char far *ram, unsigned long xoff, unsigned long d, unsigned long n)
{
    unsigned long even = n & ~1UL;
    if (even) xms_move(0, xoff, (void far *)(ram + (unsigned)d), even);
    if (n & 1UL) {
        xms_move(0, xoff + even, (void far *)s_bk_tmp, 2);
        ram[(unsigned)(d + even)] = s_bk_tmp[0];
    }
}

static int copy_file(unsigned char far *ram, unsigned long src, unsigned long d, unsigned long n)
{
    while (n > 0) {
        int k;
        unsigned long off, cnt;
        for (k = 0; k < s_bk_nseg; k++)
            if (src >= s_bk_seg[k].start && src < s_bk_seg[k].start + s_bk_seg[k].size) break;
        if (k == s_bk_nseg) return 0;
        off = src - s_bk_seg[k].start;
        cnt = s_bk_seg[k].size - off;
        if (cnt > n) cnt = n;
        if (cnt > sizeof(s_bk_tmp)) cnt = sizeof(s_bk_tmp);
        if (fseek(s_bk_file, s_bk_seg[k].foff + (long)off, SEEK_SET) != 0) return 0;
        if (fread(s_bk_tmp, 1, (unsigned)cnt, s_bk_file) != (unsigned)cnt) return 0;
        _fmemcpy(ram + (unsigned)d, s_bk_tmp, (unsigned)cnt);
        src += cnt; d += cnt; n -= cnt;
    }
    return 1;
}

static void bank_to(unsigned char far *ram, unsigned long src, unsigned long d, unsigned long n)
{
    if (s_bk_mode == BK_TIER) {
        while (n > 0) {
            unsigned long k;
            if (src < s_t1) {
                k = s_t1 - src; if (k > n) k = n;
                copy_conv(ram, src, d, k);
            } else if (src < s_t2) {
                k = s_t2 - src; if (k > n) k = n;
                copy_xms(ram, src - s_t1, d, k);
            } else {
                k = n;
                if (!copy_file(ram, src, d, k)) return;
            }
            src += k; d += k; n -= k;
        }
    } else if (s_bk_mode == BK_CONV) copy_conv(ram, src, d, n);
    else if (s_bk_mode == BK_XMS) copy_xms(ram, src, d, n);
    else copy_file(ram, src, d, n);
}

static int awe_first_chunk(unsigned long src, unsigned long d);
int g_awe_fwd = -1;

void rf5c_copy(unsigned long src, unsigned dst, unsigned long n)
{
    unsigned long d = (unsigned long)(((unsigned)s_wbank << 12) | dst);
    unsigned char far *ram;
    g_awe_fwd = -1;
    if (!s_ram_seg || s_bk_mode == BK_NONE || src >= s_bank_size || d >= 0x10000UL) return;
    if (n > s_bank_size - src) n = s_bank_size - src;
    if (n > 0x10000UL - d) n = 0x10000UL - d;
    if (s_awe_on && awe_first_chunk(src, d)) return;
    ram = (unsigned char far *)MK_FP(s_ram_seg, 0);
    bank_to(ram, src, d, n);
}

static unsigned a_vol;
void rf5c_set_boost(int on) { s_boost = on; }
void rf5c_set_half(int on)
{
    int k;
    s_half = on;
    for (k = 0; k < 8; k++) { s_ch[k].tabvol = 0xFFFFU; s_ch[k].dirty = 1; }
}

static unsigned s_mute = 0;
static void awe_remute(void);

void rf5c_set_mute(unsigned m)
{
    int k;
    for (k = 0; k < 8; k++) if ((s_mute ^ m) & (1U << k)) s_ch[k].dirty = 1;
    s_mute = m;
    awe_remute();
}

static void build_tab(int k)
{
    RfChan *c = &s_ch[k];
    unsigned vol = (s_mute & (1U << k)) ? 0 :
        (unsigned)(((unsigned long)c->env * (unsigned)((c->pan & 15) + (c->pan >> 4))) >> 4);
    c->dirty = 0;
    if (vol == c->tabvol) return;
    c->tabvol = vol;
    a_tab = s_tab0 + ((unsigned)k << 8);
    a_vol = vol;
    if (s_half) {
    _asm {
            push bx
            push cx
            push dx
            push si
            mov bx, a_tab
            mov dx, a_vol
            mov si, 1024
            mov cx, 128
        th:
            mov ax, si
            mov al, ah
            shr al, 1
            shr al, 1
            shr al, 1
            mov [bx], al
            neg al
            mov [bx+128], al
            inc bx
            add si, dx
            dec cx
            jnz th
            pop si
            pop dx
            pop cx
            pop bx
        }
    } else {
    _asm {
            push bx
            push cx
            push dx
            push si
            mov bx, a_tab
            mov dx, a_vol
            mov si, 512
            mov cx, 128
        tl:
            mov ax, si
            mov al, ah
            shr al, 1
            shr al, 1
            mov [bx], al
            neg al
            mov [bx+128], al
            inc bx
            add si, dx
            dec cx
            jnz tl
            pop si
            pop dx
            pop cx
            pop bx
        }
    }
}

static void rf_chan_asm(void)
{
    _asm {
        push bx
        push cx
        push dx
        push es
        push si
        push di
        mov es, a_rseg
        mov cx, a_n
        mov di, a_mix
        mov si, a_idx
        mov dx, a_frac
        mov bx, a_tab
        mov ax, a_stepf
        push bp
        mov bp, ax
        cmp cx, cx
        je  rstart
    rmarka:
        mov si, a_ls
        xor dx, dx
        mov al, es:[si]
        cmp al, 0FFh
        jne rconta
        mov byte ptr a_stop, 1
        cmp al, al
        je  rdone
    rmarkb:
        mov si, a_ls
        xor dx, dx
        mov al, es:[si]
        cmp al, 0FFh
        jne rcontb
        mov byte ptr a_stop, 1
        cmp al, al
        je  rdone
    rmarko:
        mov si, a_ls
        xor dx, dx
        mov al, es:[si]
        cmp al, 0FFh
        jne rconto
        mov byte ptr a_stop, 1
        cmp al, al
        je  rdone
    rstart:
        shr cx, 1
        jnc rpairs
        mov al, es:[si]
        cmp al, 0FFh
        je  rmarko
    rconto:
        mov bl, al
        mov al, [bx]
        cbw
        add [di], ax
        inc di
        inc di
        add dx, bp
        adc si, a_stepi
    rpairs:
        test cx, cx
        jz  rdone
    rlp:
        mov al, es:[si]
        cmp al, 0FFh
        je  rmarka
    rconta:
        mov bl, al
        mov al, [bx]
        cbw
        add [di], ax
        add dx, bp
        adc si, a_stepi
        mov al, es:[si]
        cmp al, 0FFh
        je  rmarkb
    rcontb:
        mov bl, al
        mov al, [bx]
        cbw
        add [di+2], ax
        add dx, bp
        adc si, a_stepi
        add di, 4
        dec cx
        jnz rlp
    rdone:
        mov ax, bp
        pop bp
        mov a_idx, si
        mov a_frac, dx
        pop di
        pop si
        pop es
        pop dx
        pop cx
        pop bx
    }
}

static void rf_out_asm(void)
{
    _asm {
        push bx
        push cx
        push dx
        push es
        push si
        push di
        mov cx, a_n
        mov si, a_mix
        les di, a_out
        cmp cx, cx
        je  olp
    oclip:
        sar ax, 15
        not al
        stosb
        dec cx
        jz  odone
    olp:
        lodsw
        add ax, 128
        cmp ax, 255
        ja  oclip
        stosb
        dec cx
        jnz olp
    odone:
        pop di
        pop si
        pop es
        pop dx
        pop cx
        pop bx
    }
}

static void rf_out_asm15(void)
{
    _asm {
        push bx
        push cx
        push dx
        push es
        push si
        push di
        mov cx, a_n
        mov si, a_mix
        les di, a_out
        cmp cx, cx
        je  plp
    pclip:
        sar ax, 15
        not al
        stosb
        dec cx
        jz  pdone
    plp:
        lodsw
        mov dx, ax
        sar dx, 1
        add ax, dx
        add ax, 128
        cmp ax, 255
        ja  pclip
        stosb
        dec cx
        jnz plp
    pdone:
        pop di
        pop si
        pop es
        pop dx
        pop cx
        pop bx
    }
}

static void rf_fast_asm(void)
{
    _asm {
        push bx
        push cx
        push dx
        push es
        push si
        push di
        mov es, a_rseg
        mov cx, a_n
        mov di, a_mix
        mov si, a_idx
        mov dx, a_frac
        mov bx, a_tab
        mov ax, a_stepf
        push bp
        mov bp, ax
        shr cx, 1
        jnc fpairs
        mov bl, es:[si]
        mov al, [bx]
        cbw
        add [di], ax
        inc di
        inc di
        add dx, bp
        adc si, a_stepi
    fpairs:
        test cx, cx
        jz  fdone
    flp:
        mov bl, es:[si]
        mov al, [bx]
        cbw
        add [di], ax
        add dx, bp
        adc si, a_stepi
        mov bl, es:[si]
        mov al, [bx]
        cbw
        add [di+2], ax
        add dx, bp
        adc si, a_stepi
        add di, 4
        dec cx
        jnz flp
    fdone:
        pop bp
        mov a_idx, si
        mov a_frac, dx
        pop di
        pop si
        pop es
        pop dx
        pop cx
        pop bx
    }
}

static unsigned char s_t16mem[17 * 256];
static unsigned s_t16 = 0;
static unsigned s_tv16[8];

static void build_tab16(int k)
{
    RfChan *c = &s_ch[k];
    unsigned vol = (s_mute & (1U << k)) ? 0 :
        (unsigned)(((unsigned long)c->env * (unsigned)((c->pan & 15) + (c->pan >> 4))) >> 4);
    unsigned char *lo, *hi;
    unsigned acc = s_half ? 64 : 32, m;
    c->dirty = 0;
    if (vol == s_tv16[k]) return;
    s_tv16[k] = vol;
    lo = (unsigned char *)(s_t16 + ((unsigned)k << 9));
    hi = lo + 256;
    for (m = 0; m < 128; m++) {
        unsigned v = s_half ? (acc >> 7) : (acc >> 6), nv = 0U - v;
        lo[m] = (unsigned char)v; hi[m] = (unsigned char)(v >> 8);
        lo[128 + m] = (unsigned char)nv; hi[128 + m] = (unsigned char)(nv >> 8);
        acc += vol;
    }
}

static void rf_fast16_asm(void)
{
    _asm {
        push bx
        push cx
        push dx
        push es
        push si
        push di
        mov es, a_rseg
        mov cx, a_n
        mov di, a_mix
        mov si, a_idx
        mov dx, a_frac
        mov bx, a_tab
        mov ax, a_stepf
        push bp
        mov bp, ax
        shr cx, 1
        jnc gpairs
        mov bl, es:[si]
        mov al, [bx]
        mov ah, [bx+256]
        add [di], ax
        inc di
        inc di
        add dx, bp
        adc si, a_stepi
    gpairs:
        test cx, cx
        jz  gdone
    glp:
        mov bl, es:[si]
        mov al, [bx]
        mov ah, [bx+256]
        add [di], ax
        add dx, bp
        adc si, a_stepi
        mov bl, es:[si]
        mov al, [bx]
        mov ah, [bx+256]
        add [di+2], ax
        add dx, bp
        adc si, a_stepi
        add di, 4
        dec cx
        jnz glp
    gdone:
        pop bp
        mov a_idx, si
        mov a_frac, dx
        pop di
        pop si
        pop es
        pop dx
        pop cx
        pop bx
    }
}

static void rf_fast_asm0(void)
{
    _asm {
        push bx
        push cx
        push dx
        push es
        push si
        push di
        mov es, a_rseg
        mov cx, a_n
        mov di, a_mix
        mov si, a_idx
        mov dx, a_frac
        mov bx, a_tab
        mov ax, a_stepf
        push bp
        mov bp, ax
        shr cx, 1
        jnc z0fpairs
        mov bl, es:[si]
        mov al, [bx]
        cbw
        add [di], ax
        inc di
        inc di
        add dx, bp
        adc si, 0
    z0fpairs:
        test cx, cx
        jz  z0fdone
    z0flp:
        mov bl, es:[si]
        mov al, [bx]
        cbw
        add [di], ax
        add dx, bp
        adc si, 0
        mov bl, es:[si]
        mov al, [bx]
        cbw
        add [di+2], ax
        add dx, bp
        adc si, 0
        add di, 4
        dec cx
        jnz z0flp
    z0fdone:
        pop bp
        mov a_idx, si
        mov a_frac, dx
        pop di
        pop si
        pop es
        pop dx
        pop cx
        pop bx
    }
}

static void rf_fast_asm1(void)
{
    _asm {
        push bx
        push cx
        push dx
        push es
        push si
        push di
        mov es, a_rseg
        mov cx, a_n
        mov di, a_mix
        mov si, a_idx
        mov dx, a_frac
        mov bx, a_tab
        mov ax, a_stepf
        push bp
        mov bp, ax
        shr cx, 1
        jnc z1fpairs
        mov bl, es:[si]
        mov al, [bx]
        cbw
        add [di], ax
        inc di
        inc di
        add dx, bp
        adc si, 1
    z1fpairs:
        test cx, cx
        jz  z1fdone
    z1flp:
        mov bl, es:[si]
        mov al, [bx]
        cbw
        add [di], ax
        add dx, bp
        adc si, 1
        mov bl, es:[si]
        mov al, [bx]
        cbw
        add [di+2], ax
        add dx, bp
        adc si, 1
        add di, 4
        dec cx
        jnz z1flp
    z1fdone:
        pop bp
        mov a_idx, si
        mov a_frac, dx
        pop di
        pop si
        pop es
        pop dx
        pop cx
        pop bx
    }
}

static void rf_fast16_asm0(void)
{
    _asm {
        push bx
        push cx
        push dx
        push es
        push si
        push di
        mov es, a_rseg
        mov cx, a_n
        mov di, a_mix
        mov si, a_idx
        mov dx, a_frac
        mov bx, a_tab
        mov ax, a_stepf
        push bp
        mov bp, ax
        shr cx, 1
        jnc y0gpairs
        mov bl, es:[si]
        mov al, [bx]
        mov ah, [bx+256]
        add [di], ax
        inc di
        inc di
        add dx, bp
        adc si, 0
    y0gpairs:
        test cx, cx
        jz  y0gdone
    y0glp:
        mov bl, es:[si]
        mov al, [bx]
        mov ah, [bx+256]
        add [di], ax
        add dx, bp
        adc si, 0
        mov bl, es:[si]
        mov al, [bx]
        mov ah, [bx+256]
        add [di+2], ax
        add dx, bp
        adc si, 0
        add di, 4
        dec cx
        jnz y0glp
    y0gdone:
        pop bp
        mov a_idx, si
        mov a_frac, dx
        pop di
        pop si
        pop es
        pop dx
        pop cx
        pop bx
    }
}

static void rf_fast16_asm1(void)
{
    _asm {
        push bx
        push cx
        push dx
        push es
        push si
        push di
        mov es, a_rseg
        mov cx, a_n
        mov di, a_mix
        mov si, a_idx
        mov dx, a_frac
        mov bx, a_tab
        mov ax, a_stepf
        push bp
        mov bp, ax
        shr cx, 1
        jnc y1gpairs
        mov bl, es:[si]
        mov al, [bx]
        mov ah, [bx+256]
        add [di], ax
        inc di
        inc di
        add dx, bp
        adc si, 1
    y1gpairs:
        test cx, cx
        jz  y1gdone
    y1glp:
        mov bl, es:[si]
        mov al, [bx]
        mov ah, [bx+256]
        add [di], ax
        add dx, bp
        adc si, 1
        mov bl, es:[si]
        mov al, [bx]
        mov ah, [bx+256]
        add [di+2], ax
        add dx, bp
        adc si, 1
        add di, 4
        dec cx
        jnz y1glp
    y1gdone:
        pop bp
        mov a_idx, si
        mov a_frac, dx
        pop di
        pop si
        pop es
        pop dx
        pop cx
        pop bx
    }
}

static const unsigned char far *a_in8;
static int far *a_out16;

static void rf_out16_x24(void)
{
    _asm {
        push bx
        push cx
        push dx
        push es
        push si
        push di
        push ds
        mov cx, a_n
        mov si, a_mix
        les di, a_out16
        lds bx, a_in8
    oalp:
        mov ax, ss:[si]
        cmp ax, 1365
        jle oac1
        mov ax, 1365
    oac1:
        cmp ax, -1365
        jge oac2
        mov ax, -1365
    oac2:
        mov dx, ax
        shl ax, 1
        add ax, dx
        shl ax, 1
        shl ax, 1
        shl ax, 1
        mov dl, [bx]
        sub dl, 128
        mov dh, dl
        xor dl, dl
        add ax, dx
        jno oac3
        mov ax, 32767
        test dh, 80h
        jz  oac3
        mov ax, 8000h
    oac3:
        stosw
        inc bx
        inc si
        inc si
        dec cx
        jnz oalp
        pop ds
        pop di
        pop si
        pop es
        pop dx
        pop cx
        pop bx
    }
}

static void rf_out16_x16(void)
{
    _asm {
        push bx
        push cx
        push dx
        push es
        push si
        push di
        push ds
        mov cx, a_n
        mov si, a_mix
        les di, a_out16
        lds bx, a_in8
    oblp:
        mov ax, ss:[si]
        cmp ax, 2047
        jle obc1
        mov ax, 2047
    obc1:
        cmp ax, -2047
        jge obc2
        mov ax, -2047
    obc2:
        shl ax, 1
        shl ax, 1
        shl ax, 1
        shl ax, 1
        mov dl, [bx]
        sub dl, 128
        mov dh, dl
        xor dl, dl
        add ax, dx
        jno obc3
        mov ax, 32767
        test dh, 80h
        jz  obc3
        mov ax, 8000h
    obc3:
        stosw
        inc bx
        inc si
        inc si
        dec cx
        jnz oblp
        pop ds
        pop di
        pop si
        pop es
        pop dx
        pop cx
        pop bx
    }
}

static void rf_chan16_asm(void)
{
    _asm {
        push bx
        push cx
        push dx
        push es
        push si
        push di
        mov es, a_rseg
        mov cx, a_n
        mov di, a_mix
        mov si, a_idx
        mov dx, a_frac
        mov bx, a_tab
        mov ax, a_stepf
        push bp
        mov bp, ax
    qlp:
        mov al, es:[si]
        cmp al, 0FFh
        je  qmark
    qcont:
        mov bl, al
        mov al, [bx]
        mov ah, [bx+256]
        add [di], ax
        inc di
        inc di
        add dx, bp
        adc si, a_stepi
        dec cx
        jnz qlp
        cmp cx, cx
        je  qdone
    qmark:
        mov si, a_ls
        xor dx, dx
        mov al, es:[si]
        cmp al, 0FFh
        jne qcont
        mov byte ptr a_stop, 1
    qdone:
        pop bp
        mov a_idx, si
        mov a_frac, dx
        pop di
        pop si
        pop es
        pop dx
        pop cx
        pop bx
    }
}

static int rf_silent_skip(RfChan *c, unsigned k, unsigned n)
{
    unsigned long f, i;
    if (!s_boost) return 0;
    if (!(s_mute & (1U << k)) && (((unsigned long)c->env * (unsigned)((c->pan & 15) + (c->pan >> 4))) >> 4) != 0) return 0;
    f = (unsigned long)c->frac + (unsigned long)c->stepf * n;
    i = (unsigned long)c->idx + (unsigned long)c->stepi * n + (f >> 16);
    c->frac = (unsigned)(f & 0xFFFFUL);
    {
        unsigned base = c->idx & 0xE000U;
        unsigned long endp = (unsigned long)(base | 0x1FFFU);
        if (i >= endp) c->idx = c->ls + (unsigned)((i - endp) & 0x1FFFUL);
        else c->idx = (unsigned)i;
    }
    return 1;
}

void rf5c_run16(const unsigned char far *in8, int far *out16, unsigned n)
{
    int k;
    if (!s_t16) {
        s_t16 = ((unsigned)s_t16mem + 255U) & 0xFF00U;
        for (k = 0; k < 8; k++) s_tv16[k] = 0xFFFFU;
    }
    while (n > 0) {
        unsigned chunk = (n > 512U) ? 512U : n;
        memset(s_mix, 0, chunk * sizeof(int));
        if (s_enable && s_ram_seg) {
            for (k = 0; k < 8; k++) {
                RfChan *c = &s_ch[k];
                unsigned done = 0;
                if (!c->on || c->awe) continue;
                if (rf_silent_skip(c, k, chunk)) continue;
                build_tab16(k);
                a_idx = c->idx; a_frac = c->frac;
                a_stepi = c->stepi; a_stepf = c->stepf;
                a_tab = s_t16 + ((unsigned)k << 9);
                a_rseg = s_ram_seg;
                if (!s_boost) {
                    a_n = chunk; a_ls = c->ls; a_mix = (unsigned)s_mix; a_stop = 0;
                    rf_chan16_asm();
                    c->idx = a_idx; c->frac = a_frac;
                    continue;
                }
                while (done < chunk) {
                    unsigned base = a_idx & 0xE000U, endp = base | 0x1FFFU;
                    unsigned m = chunk - done, maxn = (endp - a_idx) / (a_stepi + 1);
                    if (maxn == 0) maxn = 1;
                    if (m > maxn) m = maxn;
                    a_n = m; a_mix = (unsigned)s_mix + done * 2;
                    if (a_stepi == 0) rf_fast16_asm0(); else if (a_stepi == 1) rf_fast16_asm1(); else rf_fast16_asm();
                    if ((a_idx & 0xE000U) != base || a_idx == endp)
                        a_idx = c->ls + ((a_idx - endp) & 0x1FFFU);
                    done += m;
                }
                c->idx = a_idx; c->frac = a_frac;
            }
        }
        a_n = chunk; a_mix = (unsigned)s_mix; a_in8 = in8; a_out16 = out16;
        if (s_boost) rf_out16_x24(); else rf_out16_x16();
        in8 += chunk; out16 += chunk; n -= chunk;
    }
}

static void rf_outadd_asm(void)
{
    _asm {
        push bx
        push cx
        push dx
        push es
        push si
        push di
        mov cx, a_n
        mov si, a_mix
        les di, a_out
        xor bh, bh
        cmp cx, cx
        je  qlp
    qclip:
        sar ax, 15
        not al
        stosb
        dec cx
        jz  qdone
    qlp:
        lodsw
        mov bl, es:[di]
        add ax, bx
        cmp ax, 255
        ja  qclip
        stosb
        dec cx
        jnz qlp
    qdone:
        pop di
        pop si
        pop es
        pop dx
        pop cx
        pop bx
    }
}

static void rf_outadd_asm15(void)
{
    _asm {
        push bx
        push cx
        push dx
        push es
        push si
        push di
        mov cx, a_n
        mov si, a_mix
        les di, a_out
        xor bh, bh
        cmp cx, cx
        je  rlp2
    rclip2:
        sar ax, 15
        not al
        stosb
        dec cx
        jz  rdone2
    rlp2:
        lodsw
        mov dx, ax
        sar dx, 1
        add ax, dx
        mov bl, es:[di]
        add ax, bx
        cmp ax, 255
        ja  rclip2
        stosb
        dec cx
        jnz rlp2
    rdone2:
        pop di
        pop si
        pop es
        pop dx
        pop cx
        pop bx
    }
}

static int s_add = 0;

void rf5c_run(unsigned char far *out, unsigned n)
{
    while (n > 0) {
        unsigned chunk = (n > 512U) ? 512U : n;
        int k;
        memset(s_mix, 0, chunk * sizeof(int));
        if (s_enable && s_ram_seg) {
            for (k = 0; k < 8; k++) {
                RfChan *c = &s_ch[k];
                if (!c->on || c->awe) continue;
                if (rf_silent_skip(c, k, chunk)) continue;
                if (c->dirty) build_tab(k);
                a_n = chunk; a_idx = c->idx; a_frac = c->frac;
                a_stepi = c->stepi; a_stepf = c->stepf; a_ls = c->ls;
                a_tab = s_tab0 + ((unsigned)k << 8);
                a_mix = (unsigned)s_mix; a_rseg = s_ram_seg; a_stop = 0;
                if (s_boost) {
                    unsigned done = 0;
                    while (done < chunk) {
                        unsigned base = a_idx & 0xE000U, endp = base | 0x1FFFU;
                        unsigned n = chunk - done, maxn = (endp - a_idx) / (a_stepi + 1);
                        if (maxn == 0) maxn = 1;
                        if (n > maxn) n = maxn;
                        a_n = n; a_mix = (unsigned)s_mix + done * 2;
                        if (a_stepi == 0) rf_fast_asm0(); else if (a_stepi == 1) rf_fast_asm1(); else rf_fast_asm();
                        if ((a_idx & 0xE000U) != base || a_idx == endp)
                            a_idx = c->ls + ((a_idx - endp) & 0x1FFFU);
                        done += n;
                    }
                    c->idx = a_idx; c->frac = a_frac;
                    continue;
                }
                rf_chan_asm();
                c->idx = a_idx; c->frac = a_frac;
            }
        }
        a_n = chunk; a_mix = (unsigned)s_mix; a_out = out;
        if (s_add) { if (s_boost) rf_outadd_asm15(); else rf_outadd_asm(); }
        else if (s_boost) rf_out_asm15(); else rf_out_asm();
        out += chunk;
        n -= chunk;
    }
}

unsigned rf5c_active(void)
{
    unsigned m = 0;
    int k;
    if (!s_enable) return 0;
    for (k = 0; k < 8; k++)
        if (s_ch[k].on && s_ch[k].env && s_ch[k].pan) m |= 1U << k;
    return m;
}

void rf5c_run_add(unsigned char far *out, unsigned n)
{
    s_add = 1;
    rf5c_run(out, n);
    s_add = 0;
}

#define AWE_MAXENT 400
#define AWE_TABMAX (2U + AWE_MAXENT * 14U)
typedef struct { unsigned long off, len, addr; long loop; unsigned w; } AweEnt;
static AweEnt far *s_ent = 0;
static unsigned far *s_sidx = 0;
static unsigned char far *s_tbuf = 0;
static unsigned s_tseg_e = 0, s_tseg_i = 0, s_tseg_t = 0;
static unsigned s_nent = 0;
static unsigned s_tpos = 0;
static unsigned long s_awe_words = 0, s_awe_used = 0;
static unsigned s_awe_nres = 0;
static int s_awe_hw = 0;
static unsigned s_awe_port = 0;

static unsigned char hw_env[8], hw_pan[8], hw_on[8], hw_awe[8];
static unsigned hw_fd[8];
static int hw_ring[8];
static unsigned hw_cb = 0;
static unsigned char s_ub[512];
static short s_us[512];

static unsigned long rdf32(const unsigned char far *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static void awe_release(void)
{
    if (s_tseg_e) { _dos_freemem(s_tseg_e); s_tseg_e = 0; }
    if (s_tseg_i) { _dos_freemem(s_tseg_i); s_tseg_i = 0; }
    if (s_tseg_t) { _dos_freemem(s_tseg_t); s_tseg_t = 0; }
    s_ent = 0; s_sidx = 0; s_tbuf = 0; s_nent = 0;
    s_awe_on = 0;
}

static void awe_stop_all(void)
{
    int k;
    if (s_awe_hw)
        for (k = 0; k < 8; k++) if (hw_awe[k]) emu8000_note_off(k);
    memset(hw_awe, 0, sizeof(hw_awe)); memset(hw_on, 0, sizeof(hw_on));
    memset(s_rawe, 0, sizeof(s_rawe));
    for (k = 0; k < 8; k++) hw_ring[k] = -1;
}

int rf5c_awe_on(void) { return s_awe_on; }

void rf5c_awe_stats(unsigned *nres, unsigned long *kb)
{
    *nres = s_awe_nres; *kb = s_awe_used / 512UL;
}

int rf5c_awe_enable(unsigned port, unsigned kb)
{
    unsigned seg;
    awe_stop_all();
    awe_release();
    if (!port || !kb) { return 0; }
    if (!s_awe_hw || s_awe_port != port) {
        if (!emu8000_init(port)) { return 0; }
        s_awe_hw = 1; s_awe_port = port;
    }
    if (_dos_allocmem((unsigned)((AWE_MAXENT * (unsigned long)sizeof(AweEnt) + 15UL) >> 4), &seg) != 0) { return 0; }
    s_tseg_e = seg; s_ent = (AweEnt far *)MK_FP(seg, 0);
    if (_dos_allocmem((unsigned)((AWE_MAXENT * 2UL + 15UL) >> 4), &seg) != 0) { awe_release(); return 0; }
    s_tseg_i = seg; s_sidx = (unsigned far *)MK_FP(seg, 0);
    if (_dos_allocmem((AWE_TABMAX + 15U) >> 4, &seg) != 0) { awe_release(); return 0; }
    s_tseg_t = seg; s_tbuf = (unsigned char far *)MK_FP(seg, 0);
    s_awe_words = (unsigned long)kb * 512UL;
    s_awe_used = 0; s_awe_nres = 0; s_nent = 0; s_tpos = 0;
    s_awe_on = 1;
    return 1;
}

void rf5c_awe_table_begin(void) { s_tpos = 0; }

void rf5c_awe_table_data(const unsigned char *p, unsigned n)
{
    if (!s_awe_on) return;
    if (n > AWE_TABMAX - s_tpos) n = AWE_TABMAX - s_tpos;
    _fmemcpy(s_tbuf + s_tpos, p, n);
    s_tpos += n;
}

static void awe_upload_one(AweEnt far *e)
{
    unsigned long base = EMU8000_DRAM_START + s_awe_used, pos = 0, len = e->len;
    unsigned cn, j;
    while (pos < len) {
        cn = (len - pos > 512UL) ? 512U : (unsigned)(len - pos);
        bank_to((unsigned char far *)s_ub, e->off + pos, 0, cn);
        for (j = 0; j < cn; j++) {
            unsigned char b = s_ub[j];
            int v = (int)(b & 0x7F) << 8;
            s_us[j] = (short)((b & 0x80) ? -v : v);
        }
        emu8000_load_sample(base + pos, s_us, cn);
        pos += cn;
    }
    memset(s_us, 0, 48 * sizeof(short));
    if (e->loop >= 0 && (unsigned long)e->loop < len) {
        unsigned long ll = len - (unsigned long)e->loop;
        unsigned m = (ll < 48UL) ? (unsigned)ll : 48U;
        bank_to((unsigned char far *)s_ub, e->off + (unsigned long)e->loop, 0, m);
        for (j = 0; j < 48; j++) {
            unsigned char b = s_ub[j % m];
            int v = (int)(b & 0x7F) << 8;
            s_us[j] = (short)((b & 0x80) ? -v : v);
        }
    }
    emu8000_load_sample(base + len, s_us, 48);
    e->addr = base;
    s_awe_used += len + 48UL;
    s_awe_nres++;
}

void rf5c_awe_table_done(void)
{
    unsigned n, i, gap, j;
    if (!s_awe_on || s_tpos < 2 || s_bk_mode == BK_NONE) return;
    n = (unsigned)s_tbuf[0] | ((unsigned)s_tbuf[1] << 8);
    if (n > AWE_MAXENT) n = AWE_MAXENT;
    if (2U + n * 14U > s_tpos) n = (s_tpos - 2U) / 14U;
    for (i = 0; i < n; i++) {
        const unsigned char far *p = s_tbuf + 2 + i * 14U;
        s_ent[i].off = rdf32(p);
        s_ent[i].len = rdf32(p + 4);
        s_ent[i].loop = (long)rdf32(p + 8);
        s_ent[i].w = (unsigned)p[12] | ((unsigned)p[13] << 8);
        s_ent[i].addr = 0;
    }
    s_nent = n;
    for (i = 0; i < n; i++) {
        unsigned long need = s_ent[i].len + 48UL;
        if (s_ent[i].len == 0 || s_ent[i].off + s_ent[i].len > s_bank_size) continue;
        if (s_awe_used + need > s_awe_words) continue;
        awe_upload_one(&s_ent[i]);
    }
    for (i = 0; i < n; i++) s_sidx[i] = i;
    for (gap = n / 2; gap > 0; gap /= 2)
        for (i = gap; i < n; i++) {
            unsigned t = s_sidx[i];
            for (j = i; j >= gap && s_ent[s_sidx[j - gap]].off > s_ent[t].off; j -= gap) s_sidx[j] = s_sidx[j - gap];
            s_sidx[j] = t;
        }
}

static int awe_find(unsigned long src)
{
    int lo = 0, hi = (int)s_nent - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        unsigned long o = s_ent[s_sidx[mid]].off;
        if (o == src) return (int)s_sidx[mid];
        if (o < src) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

static int awe_first_chunk(unsigned long src, unsigned long d)
{
    unsigned k = (unsigned)(d >> 13) & 7U, rp = (unsigned)(d & 0x1FFFU);
    if (rp == 0) {
        int idx = awe_find(src);
        if (idx >= 0 && s_ent[idx].addr) {
            s_rawe[k] = 1;
            g_awe_fwd = (int)((k << 9) | (unsigned)(idx + 1));
            return 1;
        }
        if (s_ch[k].awe) return 1;
        s_rawe[k] = 0;
        g_awe_fwd = (int)(k << 9);
        return 0;
    }
    return s_rawe[k] != 0;
}

int g_awe_ptrim = 0;
int g_awe_gain = 8;
static unsigned awe_pitch(unsigned fd)
{
    unsigned m = 15;
    unsigned long f;
    long ip;
    if (fd == 0) fd = 1;
    while (!(fd & (1U << m))) m--;
    f = ((unsigned long)fd << (15 - m)) & 0x7FFFUL;
    ip = 0xE000L + ((long)m - 11L) * 4096L + (long)awe_log_tab[(unsigned)(f >> 5)] - 1794L + (long)g_awe_ptrim;
    if (ip < 0) ip = 0;
    if (ip > 0xFFFFL) ip = 0xFFFFL;
    return (unsigned)ip;
}

static unsigned char awe_att(int k)
{
    unsigned long v;
    if (s_mute & (1U << k)) return 255;
    v = (unsigned long)hw_env[k] * (unsigned)((hw_pan[k] & 15) + (hw_pan[k] >> 4)) / 30UL;
    if (v > 255UL) v = 255UL;
    {
        int a = (int)awe_att_tab[(unsigned)v];
        if (a >= 255) return 255;
        a -= g_awe_gain;
        return (unsigned char)(a < 0 ? 0 : a);
    }
}

static void awe_remute(void)
{
    int k;
    if (!s_awe_hw) return;
    for (k = 0; k < 8; k++) if (hw_awe[k]) emu8000_set_atten(k, awe_att(k));
}

static void awe_hw_start(int k)
{
    AweEnt far *e = &s_ent[hw_ring[k]];
    unsigned long base = e->addr, ls, le;
    if (e->loop >= 0) { ls = base + (unsigned long)e->loop; le = base + e->len; }
    else { ls = base + e->len + 4UL; le = base + e->len + 44UL; }
    emu8000_play(k, base, ls, le, awe_pitch(hw_fd[k]), awe_att(k));
    hw_awe[k] = 1;
}

void rf5c_awe_hw_ring(unsigned v)
{
    unsigned k = (v >> 9) & 7U, idx = v & 0x1FFU;
    if (!s_awe_on) return;
    hw_ring[k] = idx ? (int)idx - 1 : -1;
}

void rf5c_awe_hw_reg(unsigned reg, unsigned data)
{
    unsigned k;
    if (!s_awe_on) return;
    switch (reg) {
        case 0x00: hw_env[hw_cb] = (unsigned char)data; if (hw_awe[hw_cb]) emu8000_set_atten(hw_cb, awe_att(hw_cb)); break;
        case 0x01: hw_pan[hw_cb] = (unsigned char)data; if (hw_awe[hw_cb]) emu8000_set_atten(hw_cb, awe_att(hw_cb)); break;
        case 0x02: hw_fd[hw_cb] = (hw_fd[hw_cb] & 0xFF00U) | data; if (hw_awe[hw_cb]) emu8000_set_pitch(hw_cb, awe_pitch(hw_fd[hw_cb])); break;
        case 0x03: hw_fd[hw_cb] = (hw_fd[hw_cb] & 0x00FFU) | (data << 8); if (hw_awe[hw_cb]) emu8000_set_pitch(hw_cb, awe_pitch(hw_fd[hw_cb])); break;
        case 0x07: if (data & 0x40) hw_cb = data & 7U; break;
        case 0x08:
            for (k = 0; k < 8; k++) {
                unsigned char on = (unsigned char)((~data >> k) & 1U);
                if (on && !hw_on[k]) {
                    if (hw_ring[k] >= 0) awe_hw_start((int)k);
                } else if (!on && hw_awe[k]) {
                    emu8000_note_off((int)k);
                    hw_awe[k] = 0;
                }
                hw_on[k] = on;
            }
            break;
    }
}

