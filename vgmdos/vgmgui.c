#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <conio.h>
#include <dos.h>
#include <direct.h>
#include "vgm.h"
#include "dma.h"
#include "textui.h"
#include "vgafont.h"
#include "fastmath.h"

#include "sbdetect.h"
#include "ym2413_opl.h"
#include "swaudio.h"
#include "pgus_tandy.h"
#include "opl2.h"
#include "ym2413.h"

#include "psg_full.h"
#include "ay8910.h"
#include "scc.h"
#include "nesapu.h"
#include "gbapu.h"
#include "wsapu.h"
#include "pcengine.h"
#include "fds.h"
#include "rf5c164.h"
#include "emu8000.h"
#include "ym3812_opl.h"

#include "ym2203_opl.h"

#include <math.h>
#include "dosmem.h"
#include "dsp.h"
#include "irq.h"
#include "pctimer.h"

int g_en = 1;
#define TX(es, en) (g_en ? (en) : (es))

#define VGM_SAMPLE_RATE 44100UL
#define PLAY_SAMPLE_RATE 22050UL
#define MAX_CHUNK_VGM 512UL
#define MIX_TEMP_SIZE 512U

#define MAX_ENTRIES 700
#define MAX_FNAME 13
#define MAX_PATH_DEPTH 16
#define MAX_QUEUE 300
#define MAX_QPATH 200

enum { MACH_PSG = 0, MACH_NGP = 1, MACH_MSX = 2, MACH_MSXSCC = 3,
    MACH_NES = 4, MACH_GB = 5, MACH_PC88 = 6, MACH_WSWAN = 7, MACH_PCE = 8, MACH_OPL2 = 9, MACH_OPL1 = 10, MACH_MSXAUDIO = 11, MACH_OPL3 = 12, MACH_AY_OPL3 = 13,
    MACH_MD = 14, MACH_MCD = 15
};

typedef struct {
    PsgFull psg, psg2;
    int has_psg, dual_chip;
    Ay8910 ay; int has_ay;
    Scc scc; int has_scc;
    NesApu nes; int has_nes;
    FdsApu fds; int has_fds;
    GbApu gb; int has_gb;
    Ay8910 ay2203; int has_ay2203;
    WsApu ws; int has_ws;
    PceApu pce; int has_pce;
    int has_ym2413;
    int has_ym3812;
    int has_ym3526;
    int has_y8950;
    int has_ymf262;
    int has_dac;
    int has_ym2203fm;
    int has_rf5c;
    int rf5c_nomem;
} AllChips;

static char path_stack[MAX_PATH_DEPTH][MAX_FNAME];
static int path_depth = 0;
static unsigned long g_quality = PLAY_SAMPLE_RATE;

#define Q_CHIPS 9
enum { Q_PSG, Q_AY, Q_SCC, Q_NES, Q_FDS, Q_GB, Q_WS, Q_PCE, Q_MCD };
static unsigned long g_qcap[Q_CHIPS];
static int g_noshort = 0;

static int g_np_row_shift = 0;

static SwaudioMode g_output_mode = 0;

static int g_swaudio_active = 0;
static unsigned g_pgus_tandy_port = 0;

static int g_pgus_active = 0;

#define PSG_DOS_BOOST(v) ((((unsigned long)(v) * 181UL) >> 7) > 512UL ? 512u : (unsigned)(((unsigned long)(v) * 181UL) >> 7))

static int g_md_boost = 0;
static int g_dac_x2 = 0;
static int g_md_mixer = 0;

static int g_dac_allowed = 0;
static DosBuffer g_dac_buf;
static unsigned char far *g_dac_bank = 0;
static unsigned long g_dac_bank_size = 0;
static int g_dac_on = 0;
static int g_dac_enabled = 1;
static unsigned long g_dac_pos = 0, g_dac_end = 0, g_dac_step = 0;
static unsigned long g_dac_freq = 8000;
static unsigned long g_dac_rate = 22050;
#define g_dac_vol 16

static DosBuffer g_vdac_buf;
static unsigned char far *g_vdac = 0;
static unsigned g_vdac_n = 0;
static const unsigned char far *g_dac_src = 0;
static int g_dac_pre = 0;

static unsigned long far_u32(const unsigned char far *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static PceApu *g_pce_dac = 0;

static NesApu *g_nes_dmc = 0;
static DosBuffer g_nes_ring_buf;
static unsigned char far *g_nes_ring = 0;

static void dac_reset(unsigned long play_rate)
{
    g_pce_dac = 0;
    g_nes_dmc = 0;
    g_dac_on = 0;
    g_dac_enabled = 1;
    g_dac_bank = 0;
    g_dac_bank_size = 0;
    g_vdac = 0;
    g_vdac_n = 0;
    g_dac_src = 0;
    g_dac_pre = 0;
    g_dac_rate = play_rate;
    g_dac_freq = 8000;
    g_dac_step = (g_dac_freq << 16) / g_dac_rate;
}

static void dac_free(void)
{
    if (g_dac_bank) dosmem_free(&g_dac_buf);
    if (g_nes_ring) dosmem_free(&g_nes_ring_buf);
    g_nes_ring = 0;
    if (g_vdac) dosmem_free(&g_vdac_buf);
    g_vdac = 0;
    g_vdac_n = 0;
    g_dac_src = 0;
    g_dac_pre = 0;
    g_dac_bank = 0;
    g_dac_bank_size = 0;
    g_dac_on = 0;
}

static void dac_event(VgmFile *vgm, const VgmEvent *ev)
{
    if (!g_dac_allowed) return;
    switch (ev->type) {
        case VGM_EV_PCM_BANK:
            if (g_dac_bank || ev->wait == 0 || ev->wait > 65000UL || !vgm->pcm_fp) break;
            if (!dosmem_alloc_simple(&g_dac_buf, (unsigned)ev->wait)) break;
            g_dac_bank = g_dac_buf.ptr;
            g_dac_bank_size = ev->wait;
            {
                static unsigned char tmp[128];
                unsigned long done = 0;
                fseek(vgm->pcm_fp, vgm->pcm_bank_offset, SEEK_SET);
                while (done < g_dac_bank_size) {
                    unsigned n = (g_dac_bank_size - done > sizeof(tmp)) ? sizeof(tmp) : (unsigned)(g_dac_bank_size - done);
                    unsigned k;
                    if (fread(tmp, 1, n, vgm->pcm_fp) != n) break;
                    for (k = 0; k < n; k++) g_dac_bank[(unsigned)done + k] = tmp[k];
                    done += n;
                }
            }
            if (g_pce_dac) pceapu_set_bank(g_pce_dac, g_dac_bank, g_dac_bank_size);
            break;
        case VGM_EV_NES_RAM:
            if (!g_nes_dmc || ev->wait < 3 || !vgm->pcm_fp) break;
            if (!g_dac_bank) {
                unsigned k;
                if (!dosmem_alloc_simple(&g_dac_buf, 16384U)) break;
                g_dac_bank = g_dac_buf.ptr;
                g_dac_bank_size = 16384UL;
                for (k = 0; k < 16384U; k++) g_dac_bank[k] = 0;
                nesapu_set_ram(g_nes_dmc, g_dac_bank);
            }
            {
                static unsigned char tmp[128];
                unsigned long left = ev->wait - 2, addr;
                fseek(vgm->pcm_fp, vgm->pcm_bank_offset, SEEK_SET);
                if (fread(tmp, 1, 2, vgm->pcm_fp) != 2) break;
                addr = (unsigned long)tmp[0] | ((unsigned long)tmp[1] << 8);
                while (left > 0) {
                    unsigned n = left > sizeof(tmp) ? sizeof(tmp) : (unsigned)left;
                    unsigned k;
                    if (fread(tmp, 1, n, vgm->pcm_fp) != n) break;
                    for (k = 0; k < n; k++, addr++)
                        if (addr >= 0xC000UL && addr <= 0xFFFFUL)
                            g_dac_bank[(unsigned)(addr - 0xC000UL)] = tmp[k];
                    left -= n;
                }
            }
            break;
        case VGM_EV_NES_PCM:
            if (!g_nes_dmc || ev->wait < 3 || !vgm->pcm_fp) break;
            if (!g_nes_ring) {
                if (!dosmem_alloc_simple(&g_nes_ring_buf, NES_PCM_RING)) break;
                g_nes_ring = g_nes_ring_buf.ptr;
                nesapu_set_pcm(g_nes_dmc, g_nes_ring);
            }
            {
                static unsigned char tmp[128];
                unsigned long left = ev->wait - 2;
                unsigned pos;
                fseek(vgm->pcm_fp, vgm->pcm_bank_offset, SEEK_SET);
                if (fread(tmp, 1, 2, vgm->pcm_fp) != 2) break;
                pos = ((unsigned)tmp[0] | ((unsigned)tmp[1] << 8)) & (NES_PCM_RING - 1);
                while (left > 0) {
                    unsigned n = left > sizeof(tmp) ? sizeof(tmp) : (unsigned)left;
                    unsigned k;
                    if (fread(tmp, 1, n, vgm->pcm_fp) != n) break;
                    for (k = 0; k < n; k++) {
                        g_nes_ring[pos] = tmp[k];
                        pos = (pos + 1) & (NES_PCM_RING - 1);
                    }
                    left -= n;
                }
            }
            break;
        case VGM_EV_DAC_TABLE:
            if (g_vdac || ev->wait < 10 || ev->wait > 65000UL || !vgm->pcm_fp) break;
            {
                static unsigned char tmp[128];
                unsigned long done = 0, size = ev->wait;
                fseek(vgm->pcm_fp, vgm->vdac_offset, SEEK_SET);
                if (fread(tmp, 1, 10, vgm->pcm_fp) != 10) break;
                if (tmp[0] != 'V' || tmp[1] != 'D' || tmp[2] != 'A' || tmp[3] != 'C') break;
                if (((unsigned long)tmp[6] | ((unsigned long)tmp[7] << 8)) != g_dac_rate) break;
                if (!dosmem_alloc_simple(&g_vdac_buf, (unsigned)size)) break;
                g_vdac = g_vdac_buf.ptr;
                fseek(vgm->pcm_fp, vgm->vdac_offset, SEEK_SET);
                while (done < size) {
                    unsigned n = (size - done > sizeof(tmp)) ? sizeof(tmp) : (unsigned)(size - done);
                    unsigned k;
                    if (fread(tmp, 1, n, vgm->pcm_fp) != n) break;
                    for (k = 0; k < n; k++) g_vdac[(unsigned)done + k] = tmp[k];
                    done += n;
                }
                if (done < size) { dosmem_free(&g_vdac_buf); g_vdac = 0; break; }
                g_vdac_n = (unsigned)g_vdac[8] | ((unsigned)g_vdac[9] << 8);
                if (10UL + 24UL * g_vdac_n > size) { dosmem_free(&g_vdac_buf); g_vdac = 0; g_vdac_n = 0; }
            }
            break;
        case VGM_EV_DAC_FREQ:
            if (g_pce_dac) { pceapu_stream_freq(g_pce_dac, ev->data, ev->wait); break; }
            if (g_nes_dmc) { nesapu_stream_freq(g_nes_dmc, ev->wait); break; }
            if (ev->wait > 0 && ev->wait <= 44100UL) {
                g_dac_freq = ev->wait;
                g_dac_step = (g_dac_freq << 16) / g_dac_rate;
            }
            break;
        case VGM_EV_DAC_START: {
            unsigned long off = ev->addr, len = ev->wait, end;
            unsigned char mode = (unsigned char)(ev->data & 0x0F);
            if (g_pce_dac) { pceapu_stream_start(g_pce_dac, ev->data >> 4, off, len); break; }
            if (g_nes_dmc) { nesapu_stream_start(g_nes_dmc, off, len); break; }
            if (!g_dac_bank || off >= g_dac_bank_size) break;
            if (mode == 1) end = off + len;
            else if (mode == 2) end = off + (len * g_dac_freq) / 1000UL;
            else end = g_dac_bank_size;
            if (end > g_dac_bank_size) end = g_dac_bank_size;
            g_dac_pos = off << 16;
            g_dac_end = end << 16;
            g_dac_step = (g_dac_freq << 16) / g_dac_rate;
            g_dac_src = g_dac_bank;
            g_dac_pre = 0;
            g_dac_on = 1;
            if (g_vdac && mode == 1) {
                unsigned i;
                const unsigned char far *e = g_vdac + 10;
                for (i = 0; i < g_vdac_n; i++, e += 24) {
                    if (far_u32(e) == off && len <= far_u32(e + 4) &&
                        g_dac_freq >= far_u32(e + 8) && g_dac_freq <= far_u32(e + 12)) {
                        unsigned long doff = far_u32(e + 16), dlen = far_u32(e + 20);
                        unsigned long n = (len * g_dac_rate + g_dac_freq - 1) / g_dac_freq;
                        if (n < dlen) dlen = n;
                        g_dac_src = g_vdac;
                        g_dac_pos = doff << 16;
                        g_dac_end = (doff + dlen) << 16;
                        g_dac_step = 0x10000UL;
                        g_dac_pre = 1;
                        break;
                    }
                }
            }
            break;
        }
        case VGM_EV_DAC_STOP:
            if (g_pce_dac) { pceapu_stream_stop(g_pce_dac, ev->data); break; }
            if (g_nes_dmc) { nesapu_stream_stop(g_nes_dmc); break; }
            g_dac_on = 0;
            break;
        default:
            break;
    }
}

static int g_dtab[256];
static int g_dtab_key = -1;

static void dac_tab(int vol)
{
    int k = (vol << 2) | (g_dac_pre << 1) | (g_dac_x2 ? 1 : 0), i;
    if (k == g_dtab_key) return;
    g_dtab_key = k;
    for (i = 0; i < 256; i++) {
        int smp = i - 128;
        if (g_dac_pre) {
            if (vol != 16) smp = (smp * vol) >> 4;
        } else if (vol != 16) smp = (smp * vol * 5) >> 7;
        else smp = ((smp << 2) + smp) >> 3;
        if (g_dac_x2) smp = (smp * 13) >> 3;
        g_dtab[i] = smp;
    }
}

static void dac_mix(unsigned char far *p, unsigned n)
{
    unsigned i;
    unsigned long pos = g_dac_pos, end = g_dac_end, step = g_dac_step;
    const unsigned char far *bank = g_dac_src;
    int vol = g_dac_vol;
    if (!g_dac_enabled || vol == 0) {
        pos += step * (unsigned long)n;
    } else {
        dac_tab(vol);
        for (i = 0; i < n && pos < end; i++) {
            int m = (int)p[i] + g_dtab[bank[(unsigned)(pos >> 16)]];
            if (m > 255) m = 255;
            else if (m < 0) m = 0;
            p[i] = (unsigned char)m;
            pos += step;
        }
    }
    g_dac_pos = pos;
    if (pos >= end) g_dac_on = 0;
}

static int g_mix_saved = 0;
static unsigned g_mix_base = 0;
static unsigned char g_mix_old[6];
static const unsigned char g_mix_regs[6] = { 0x04, 0x26, 0x32, 0x33, 0x34, 0x35 };

static void sbmix_write(unsigned base, unsigned char reg, unsigned char val)
{
    outp(base + 4, reg);
    outp(base + 5, val);
}

static unsigned char sbmix_read(unsigned base, unsigned char reg)
{
    outp(base + 4, reg);
    return (unsigned char)inp(base + 5);
}

#define MIX_AUTO_VOICE 24
#define MIX_AUTO_FM    31

#define MIX_MSX_VOICE  31
#define MIX_MSX_FM     29
static int g_mix_msx = 0;

#define MIX_SOLO_VOICE 31
static int g_mix_solo = 0;
static int g_mix_pcm = 0;

static void sbmix_apply(unsigned base, int is_sb16)
{
    int i;
    int voice, fm;
    if (!is_sb16) return;
    if (g_mix_pcm && !g_mix_solo) {
        voice = 31; fm = MIX_AUTO_FM;
    } else if (g_mix_solo) {
        voice = MIX_SOLO_VOICE; fm = MIX_AUTO_FM;
    } else if (g_md_boost) {
        voice = MIX_AUTO_VOICE + 3; fm = MIX_AUTO_FM;
    } else if (g_mix_msx) {
        voice = MIX_MSX_VOICE; fm = MIX_MSX_FM;
    } else {
        voice = MIX_AUTO_VOICE; fm = MIX_AUTO_FM;
    }
    if (!g_mix_saved) {
        for (i = 0; i < 6; i++) g_mix_old[i] = sbmix_read(base, g_mix_regs[i]);
        g_mix_base = base;
        g_mix_saved = 1;
    }
    if (voice >= 0) {
        unsigned char n = (unsigned char)(voice >> 1);
        sbmix_write(base, 0x04, (unsigned char)((n << 4) | n));
        sbmix_write(base, 0x32, (unsigned char)(voice << 3));
        sbmix_write(base, 0x33, (unsigned char)(voice << 3));
    }
    if (fm >= 0) {
        unsigned char n = (unsigned char)(fm >> 1);
        sbmix_write(base, 0x26, (unsigned char)((n << 4) | n));
        sbmix_write(base, 0x34, (unsigned char)(fm << 3));
        sbmix_write(base, 0x35, (unsigned char)(fm << 3));
    }
}

static void sbmix_restore(void)
{
    int i;
    if (!g_mix_saved) return;
    for (i = 0; i < 6; i++) sbmix_write(g_mix_base, g_mix_regs[i], g_mix_old[i]);
    g_mix_saved = 0;
}

static unsigned char far *g_mx_dst;
static unsigned char *g_mx_src;
static unsigned g_mx_n;
static void mix_add_temp(unsigned char far *base, unsigned pos, unsigned char *temp, unsigned long chunk)
{
    if (!chunk) return;
    g_mx_dst = base + pos; g_mx_src = temp; g_mx_n = (unsigned)chunk;
    _asm {
        push bx
        push cx
        push es
        push si
        push di
        les di, g_mx_dst
        mov si, g_mx_src
        mov cx, g_mx_n
        xor ah, ah
        xor bh, bh
        cmp cx, cx
        je  mxlp
    mxclip:
        sar ax, 15
        not al
        stosb
        dec cx
        jz  mxdone
        xor ah, ah
    mxlp:
        mov al, es:[di]
        mov bl, [si]
        inc si
        add ax, bx
        sub ax, 128
        cmp ax, 255
        ja  mxclip
        stosb
        dec cx
        jnz mxlp
    mxdone:
        pop di
        pop si
        pop es
        pop cx
        pop bx
    }
}

static int g_bits16 = 0;
static int g_out16 = 0;
static int g_skip_rf = 0;
static int g_dma_ch = 1;
static void mix_generate8(AllChips *c, unsigned char far *base, unsigned buf_len,
                          unsigned *write_pos, unsigned long n);

static void mix_generate(AllChips *c, unsigned char far *base, unsigned buf_len,
                          unsigned *write_pos, unsigned long n)
{
    static unsigned char gen8[MIX_TEMP_SIZE];
    if (!g_out16) { mix_generate8(c, base, buf_len, write_pos, n); return; }
    while (n > 0) {
        unsigned long chunk = (unsigned long)buf_len - *write_pos;
        unsigned wp = 0;
        if (chunk > n) chunk = n;
        if (chunk > MIX_TEMP_SIZE) chunk = MIX_TEMP_SIZE;
        g_skip_rf = 1;
        mix_generate8(c, (unsigned char far *)gen8, (unsigned)chunk, &wp, chunk);
        g_skip_rf = 0;
        rf5c_run16((unsigned char far *)gen8, (int far *)base + *write_pos, (unsigned)chunk);
        *write_pos = (unsigned)((*write_pos + chunk) % buf_len);
        n -= chunk;
    }
}

static int g_psg_half = 0;
static unsigned char g_ph_last;
static int g_ph_have = 0;
static void psg_half_run(PsgFull *p, unsigned char far *out, unsigned n, unsigned char *tmp)
{
    unsigned i = 0, n2, j;
    if (g_ph_have && n) { out[0] = g_ph_last; i = 1; g_ph_have = 0; }
    if (i >= n) return;
    n2 = (n - i + 1) / 2;
    psg_full_run(p, (unsigned char far *)tmp, n2);
    for (j = 0; j < n2; j++) {
        out[i++] = tmp[j];
        if (i < n) out[i++] = tmp[j];
        else { g_ph_have = 1; g_ph_last = tmp[j]; }
    }
}

static void mix_generate8(AllChips *c, unsigned char far *base, unsigned buf_len,
                          unsigned *write_pos, unsigned long n)
{
    static unsigned char temp[MIX_TEMP_SIZE];
    while (n > 0) {
        unsigned long chunk = (unsigned long)buf_len - *write_pos;
        unsigned long i;
        int any = 0;
        if (chunk > n) chunk = n;
        if (chunk > MIX_TEMP_SIZE) chunk = MIX_TEMP_SIZE;
        if (c->has_psg) {
            if (g_pgus_active) {
            } else if (c->dual_chip) {
                psg_full_run_t6w28(&c->psg, &c->psg2, base + *write_pos, chunk);
                any = 1;
            } else if (!psg_full_silent(&c->psg)) {
                if (g_psg_half) psg_half_run(&c->psg, base + *write_pos, (unsigned)chunk, temp);
                else psg_full_run(&c->psg, base + *write_pos, chunk);
                any = 1;
            }
        }
        if (c->has_ay) {
            if (!any) { ay8910_run(&c->ay, base + *write_pos, chunk); any = 1; }
            else { ay8910_run(&c->ay, temp, chunk); mix_add_temp(base, *write_pos, temp, chunk); }
        }
        if (c->has_ay2203) {
            if (!any) { ay8910_run(&c->ay2203, base + *write_pos, chunk); any = 1; }
            else { ay8910_run(&c->ay2203, temp, chunk); mix_add_temp(base, *write_pos, temp, chunk); }
        }
        if (c->has_ws) {
            if (!any) { wsapu_run(&c->ws, base + *write_pos, chunk); any = 1; }
            else { wsapu_run(&c->ws, temp, chunk); mix_add_temp(base, *write_pos, temp, chunk); }
        }
        if (c->has_pce) {
            if (!any) { pceapu_run(&c->pce, base + *write_pos, chunk); any = 1; }
            else { pceapu_run(&c->pce, temp, chunk); mix_add_temp(base, *write_pos, temp, chunk); }
        }
        if (c->has_scc) {
            if (!any) { scc_run(&c->scc, base + *write_pos, chunk); any = 1; }
            else { scc_run_additive(&c->scc, base + *write_pos, chunk); }
        }
        if (c->has_nes) {
            if (!any) { nesapu_run(&c->nes, base + *write_pos, chunk); any = 1; }
            else { nesapu_run(&c->nes, temp, chunk); mix_add_temp(base, *write_pos, temp, chunk); }
        }
        if (c->has_fds) {
            fds_run(&c->fds, base + *write_pos, chunk, any);
            any = 1;
        }
        if (c->has_gb) {
            if (!any) { gbapu_run(&c->gb, base + *write_pos, chunk); any = 1; }
            else { gbapu_run(&c->gb, temp, chunk); mix_add_temp(base, *write_pos, temp, chunk); }
        }
        if (c->has_rf5c && !g_skip_rf) {
            if (!any) { rf5c_run(base + *write_pos, (unsigned)chunk); any = 1; }
            else rf5c_run_add(base + *write_pos, (unsigned)chunk);
        }
        if (!any) _fmemset(base + *write_pos, 128, (unsigned)chunk);
        if (g_dac_on) dac_mix(base + *write_pos, (unsigned)chunk);
        *write_pos = (unsigned)((*write_pos + chunk) % buf_len);
        n -= chunk;
    }
}

static void piano_generic_touch(unsigned char seed);

static unsigned g_min_gen = 1;

static void rf5c_event(VgmFile *vgm, VgmEvent *ev)
{
    switch (ev->type) {
        case VGM_EV_RF5C_WRITE: rf5c_write((unsigned)ev->addr, ev->data); break;
        case VGM_EV_RF5C_MEM: rf5c_mem((unsigned)ev->addr, ev->data); break;
        case VGM_EV_RF5C_RAM: {
            static unsigned char t[256];
            unsigned long left = ev->wait;
            rf5c_ram_begin((unsigned)ev->addr);
            while (left > 0) {
                unsigned k = (left > sizeof(t)) ? (unsigned)sizeof(t) : (unsigned)left;
                unsigned g = vgm_read_inline(vgm, t, k);
                rf5c_ram_data(t, g);
                if (g < k) break;
                left -= g;
            }
            break;
        }
        case VGM_EV_RF5C_BANK: rf5c_bank_load(vgm->pcm_fp, vgm->pcm_bank_offset, ev->wait); break;
        case VGM_EV_RF5C_COPY: rf5c_copy(ev->addr, vgm->rf_dst, ev->wait); break;
        case VGM_EV_RF5C_TABLE:
            if (rf5c_awe_on()) {
                static unsigned char t[256];
                unsigned long left = ev->wait;
                rf5c_awe_table_begin();
                while (left > 0) {
                    unsigned k = (left > sizeof(t)) ? (unsigned)sizeof(t) : (unsigned)left;
                    unsigned g = vgm_read_inline(vgm, t, k);
                    rf5c_awe_table_data(t, g);
                    if (g < k) break;
                    left -= g;
                }
                rf5c_awe_table_done();
            }
            break;
        default: break;
    }
}

static void route_write(AllChips *c, VgmEventType type, unsigned int addr, unsigned char data, unsigned long cur_sample)
{
    unsigned char d = data;
    unsigned int a = addr;
    piano_generic_touch((unsigned char)(d ^ (unsigned char)a));
    switch (type) {
        case VGM_EV_AWE_REG: rf5c_awe_hw_reg(a, d); break;
        case VGM_EV_AWE_RING: rf5c_awe_hw_ring(a); break;
        case VGM_EV_PSG_WRITE:
            if (g_pgus_active) { pgus_tandy_write(d); }
            else if (c->has_psg) { psg_full_write(&c->psg, d); }
            break;
        case VGM_EV_PSG2_WRITE:
            if (!g_pgus_active && c->has_psg && c->dual_chip) {
                psg_full_write(&c->psg2, d);
            }
            break;
        case VGM_EV_AY8910_WRITE:
            if (c->has_ay) ay8910_write_reg(&c->ay, a, d);
            break;
        case VGM_EV_SCC_WRITE:
            if (c->has_scc) scc_write_reg(&c->scc, a, d);
            break;
        case VGM_EV_NES_APU_WRITE:
            if (a <= 0x1F) {
                if (c->has_nes) nesapu_write_reg(&c->nes, a, d);
            } else if (c->has_fds) {
                unsigned int real_addr = (a < 0x3F) ? (a + 0x4060) : ((a == 0x3F) ? 0x4023 : (a + 0x4000));
                fds_write_reg(&c->fds, real_addr, d);
            }
            break;
        case VGM_EV_PCE_WRITE:
            if (c->has_pce) pceapu_write_reg(&c->pce, a, d);
            break;
        case VGM_EV_GB_APU_WRITE:
            if (c->has_gb) gbapu_write_reg(&c->gb, a, d);
            break;
        case VGM_EV_YM2413_WRITE:
            if (c->has_ym2413) ym2413opl_write((unsigned char)a, d);
            break;
        case VGM_EV_YM2203_WRITE:
        case VGM_EV_YM2608_WRITE: {
            unsigned int fm_port = (a & 0x100) ? 1 : 0;
            unsigned char fm_addr = (unsigned char)(a & 0xFF);
            if (fm_port == 0 && c->has_ay2203 && a <= 0x0D) {
                ay8910_write_reg(&c->ay2203, (unsigned char)a, d);
                return;
            }
            if (c->has_ym2203fm) {
                ym2203opl_write((int)fm_port, fm_addr, d);
            }
            break;
        }
        case VGM_EV_YM3812_WRITE:
            if (c->has_ym3812) ym3812opl_write((unsigned char)a, d);
            break;
        case VGM_EV_YM3526_WRITE:
            if (c->has_ym3526) ym3526opl_write((unsigned char)a, d);
            break;
        case VGM_EV_Y8950_WRITE:
            if (c->has_y8950) y8950opl_write((unsigned char)a, d);
            break;
        case VGM_EV_YMF262_WRITE:
            if (c->has_ymf262) ymf262opl_write((a & 0x100) ? 1 : 0, (unsigned char)(a & 0xFF), d);
            break;
        case VGM_EV_WSWAN_PORT:
            if (c->has_ws) wsapu_write_port(&c->ws, (unsigned char)a, d);
            break;
        case VGM_EV_WSWAN_MEM:
            if (c->has_ws) wsapu_write_mem(&c->ws, a, d);
            break;
        case VGM_EV_YM2612_WRITE:
            if (c->has_dac && a == 0x2B) g_dac_enabled = (d & 0x80) ? 1 : 0;
            break;
        default:
            break;
    }
}

#define OPLQ_SIZE 4096u
#define OPLQ_MASK (OPLQ_SIZE - 1u)
typedef struct {
    unsigned long due;
    unsigned int addr;
    unsigned char type, data;
} OplQEntry;
static OplQEntry far g_oplq[OPLQ_SIZE];
static unsigned g_oplq_head = 0, g_oplq_tail = 0;
static unsigned long g_oplq_lat = 0;

static int is_hw_write(AllChips *c, VgmEventType type, unsigned int addr)
{
    switch (type) {
        case VGM_EV_PSG_WRITE:   return g_pgus_active;
        case VGM_EV_YM2413_WRITE:
        case VGM_EV_YM3812_WRITE:
        case VGM_EV_YM3526_WRITE:
        case VGM_EV_Y8950_WRITE:
        case VGM_EV_YMF262_WRITE:
        case VGM_EV_YM2612_WRITE: return 1;
        case VGM_EV_YM2203_WRITE:
        case VGM_EV_YM2608_WRITE:
            return !((addr & 0x100) == 0 && c->has_ay2203 && addr <= 0x0D);
        default: return 0;
    }
}

#define PRE_MAX 1024u
static VgmEvent far g_pre[PRE_MAX];
static unsigned g_pre_n = 0;

static void oplq_service(AllChips *c, unsigned long now)
{
    while (g_oplq_head != g_oplq_tail) {
        OplQEntry far *e = &g_oplq[g_oplq_head];
        if ((long)(now - e->due) < 0) break;
        route_write(c, (VgmEventType)e->type, e->addr, e->data, 0);
        g_oplq_head = (g_oplq_head + 1u) & OPLQ_MASK;
    }
}

static void oplq_push(AllChips *c, VgmEventType type, unsigned int addr,
                      unsigned char data, unsigned long due)
{
    OplQEntry far *e;
    unsigned nt = (g_oplq_tail + 1u) & OPLQ_MASK;
    if (nt == g_oplq_head) {
        e = &g_oplq[g_oplq_head];
        route_write(c, (VgmEventType)e->type, e->addr, e->data, 0);
        g_oplq_head = (g_oplq_head + 1u) & OPLQ_MASK;
    }
    e = &g_oplq[g_oplq_tail];
    e->due = due; e->addr = addr; e->type = (unsigned char)type; e->data = data;
    g_oplq_tail = nt;
}

static void awe_forward(AllChips *c, const VgmEvent *ev, unsigned long due)
{
    VgmEventType ty;
    unsigned int a;
    unsigned char d = 0;
    if (!rf5c_awe_on()) return;
    if (ev->type == VGM_EV_RF5C_WRITE) { ty = VGM_EV_AWE_REG; a = (unsigned)ev->addr; d = ev->data; }
    else if (ev->type == VGM_EV_RF5C_COPY && g_awe_fwd >= 0) { ty = VGM_EV_AWE_RING; a = (unsigned)g_awe_fwd; }
    else return;
    if (g_oplq_lat && due) oplq_push(c, ty, a, d, due);
    else route_write(c, ty, a, d, 0);
}

static void awe_try_enable(void)
{
    char *a = getenv("AWE32"), *b = getenv("BLASTER");
    unsigned kb, port = 0;
    if (!a || !b) return;
    kb = (unsigned)atoi(a);
    if (kb == 0) return;
    if (kb == 1) kb = 512;
    while (*b) {
        while (*b == ' ') b++;
        if ((*b == 'E' || *b == 'e') && b[1]) port = (unsigned)strtoul(b + 1, NULL, 16);
        while (*b && *b != ' ') b++;
    }
    if (port) rf5c_awe_enable(port, kb);
}

static int detect_machine_from_stream(const char *fname)
{
    VgmFile vgm;
    VgmEvent ev;
    int seen_psg = 0, seen_psg2 = 0, seen_ay = 0, seen_scc = 0, seen_nes = 0, seen_gb = 0, seen_ym2203 = 0, seen_ws = 0, seen_ym2608 = 0, seen_pce = 0;
    int seen_ym3812 = 0, seen_ym3526 = 0, seen_y8950 = 0, seen_ymf262 = 0;
    int seen_ym2612 = 0, seen_rf5c = 0;
    unsigned n_events = 0;
#define DETECT_PSG_ONLY_CAP 800
#define DETECT_AY_ONLY_CAP 800
    if (vgm_open(&vgm, fname) != 0) return MACH_PSG;
    while (vgm_next_event(&vgm, &ev) == 0) {
        n_events++;
        switch (ev.type) {
            case VGM_EV_PSG_WRITE: seen_psg = 1; break;
            case VGM_EV_PSG2_WRITE: seen_psg2 = 1; break;
            case VGM_EV_AY8910_WRITE: seen_ay = 1; break;
            case VGM_EV_SCC_WRITE: seen_scc = 1; break;
            case VGM_EV_NES_APU_WRITE: seen_nes = 1; break;
            case VGM_EV_GB_APU_WRITE: seen_gb = 1; break;
            case VGM_EV_YM2203_WRITE: seen_ym2203 = 1; break;
            case VGM_EV_YM2608_WRITE: seen_ym2608 = 1; break;
            case VGM_EV_WSWAN_PORT:
            case VGM_EV_WSWAN_MEM: seen_ws = 1; break;
            case VGM_EV_PCE_WRITE: seen_pce = 1; break;
            case VGM_EV_YM3812_WRITE: seen_ym3812 = 1; break;
            case VGM_EV_YM3526_WRITE: seen_ym3526 = 1; break;
            case VGM_EV_Y8950_WRITE: seen_y8950 = 1; break;
            case VGM_EV_YMF262_WRITE: seen_ymf262 = 1; break;
            case VGM_EV_YM2612_WRITE: seen_ym2612 = 1; break;
            case VGM_EV_RF5C_WRITE:
            case VGM_EV_RF5C_RAM:
            case VGM_EV_RF5C_COPY: seen_rf5c = 1; break;
            default: break;
        }
        if (seen_nes || seen_gb || seen_ws || seen_pce ||
            seen_ym2203 || seen_ym2608 || seen_scc || seen_psg2 ||
            seen_ym3812 || seen_ym3526 || seen_y8950)
            break;
        if (seen_ym2612 && n_events >= 64) break;
        if (seen_ymf262 && seen_ay) break;
        if (seen_ay || seen_ymf262) {
            if (n_events >= DETECT_AY_ONLY_CAP) break;
        } else if (n_events >= DETECT_PSG_ONLY_CAP) break;
    }
    vgm_close(&vgm);
    if (seen_ym2612 && !seen_ymf262) return MACH_MD;
    if (seen_rf5c && !seen_ymf262) return MACH_MCD;
    if (seen_nes) return MACH_NES;
    if (seen_gb) return MACH_GB;
    if (seen_ws) return MACH_WSWAN;
    if (seen_pce) return MACH_PCE;
    if (seen_ym2203 || seen_ym2608) return MACH_PC88;
    if (seen_scc) return MACH_MSXSCC;
    if (seen_ay && seen_ymf262) return MACH_AY_OPL3;
    if (seen_ay) return MACH_MSX;
    if (seen_ymf262) return MACH_OPL3;
    if (seen_y8950) return MACH_MSXAUDIO;
    if (seen_ym3812) return MACH_OPL2;
    if (seen_ym3526) return MACH_OPL1;
    if (seen_psg2) return MACH_NGP;
    if (seen_psg) return MACH_PSG;
    return MACH_PSG;
}
#undef DETECT_PSG_ONLY_CAP
#undef DETECT_AY_ONLY_CAP

static int detect_machine(const char *fname)
{
    return detect_machine_from_stream(fname);
}

#define MAX_ALTNAME VGM_TAG_MAXLEN

typedef struct {
    char name[MAX_FNAME];
    int is_dir;
    char gd3_title[VGM_TAG_MAXLEN];
    int gd3_tried;
} Entry;
#define alt_name gd3_title

typedef struct {
    Entry items[MAX_ENTRIES];
    int count;
} EntryList;

static void far_name_copy(char *dst, const char far *src, int maxlen);

static int entry_cmp(const Entry far *e, int is_dir, const char *name)
{
    char nb[MAX_FNAME];
    int k;
    if (e->is_dir != is_dir) return e->is_dir ? -1 : 1;
    for (k = 0; k < MAX_FNAME - 1 && e->name[k]; k++) nb[k] = e->name[k];
    nb[k] = '\0';
    return stricmp(nb, name);
}

static void apply_name_list(EntryList far *el, const char *fname, int dirs)
{
    FILE *f = fopen(fname, "r");
    char line[128];
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        char *k = line, *v;
        int kn, vn, lo, hi;
        if (!eq) continue;
        *eq = '\0';
        v = eq + 1;
        kn = (int)strlen(k);
        while (kn > 0 && (k[kn-1] == ' ' || k[kn-1] == '\t')) k[--kn] = '\0';
        while (*k == ' ' || *k == '\t') { k++; kn--; }
        while (*v == ' ' || *v == '\t') v++;
        vn = (int)strlen(v);
        while (vn > 0 && (v[vn-1] == '\n' || v[vn-1] == '\r' || v[vn-1] == ' ' || v[vn-1] == '\t'))
            v[--vn] = '\0';
        if (kn <= 0 || kn >= MAX_FNAME || vn <= 0) continue;
        if (vn >= VGM_TAG_MAXLEN) { v[VGM_TAG_MAXLEN - 1] = '\0'; vn = VGM_TAG_MAXLEN - 1; }
        lo = 0;
        hi = el->count - 1;
        while (lo <= hi) {
            int mid = (lo + hi) >> 1;
            int c = entry_cmp(&el->items[mid], dirs, k);
            if (c == 0) {
                int q;
                for (q = 0; q <= vn; q++) el->items[mid].gd3_title[q] = v[q];
                el->items[mid].gd3_tried = 1;
                break;
            }
            if (c < 0) lo = mid + 1; else hi = mid - 1;
        }
    }
    fclose(f);
}

static void scan_entries(EntryList far *el)
{
    struct find_t ff;
    Entry tmp;
    #define SCAN_ATTR_MASK 0x37
    el->count = 0;
    if (_dos_findfirst("*.*", SCAN_ATTR_MASK, &ff) == 0) {
        do {
            if (strcmp(ff.name, ".") == 0 || strcmp(ff.name, "..") == 0) continue;
            if (el->count >= MAX_ENTRIES) break;
            if (ff.attrib & _A_SUBDIR) {
                memset(&tmp, 0, sizeof(tmp));
                strncpy(tmp.name, ff.name, MAX_FNAME - 1);
                tmp.name[MAX_FNAME - 1] = '\0';
                tmp.is_dir = 1;
                tmp.gd3_tried = 1;
                el->items[el->count] = tmp;
                el->count++;
            } else {
                int len = (int)strlen(ff.name);
                if (len > 4 && stricmp(ff.name + len - 4, ".vgm") == 0) {
                    memset(&tmp, 0, sizeof(tmp));
                    strncpy(tmp.name, ff.name, MAX_FNAME - 1);
                    tmp.name[MAX_FNAME - 1] = '\0';
                    tmp.is_dir = 0;
                    el->items[el->count] = tmp;
                    el->count++;
                }
            }
        } while (_dos_findnext(&ff) == 0);
    }
    {
        int i, j;
        for (i = 1; i < el->count; i++) {
            Entry t = el->items[i];
            j = i - 1;
            while (j >= 0 && entry_cmp(&el->items[j], t.is_dir, t.name) > 0) {
                el->items[j + 1] = el->items[j];
                j--;
            }
            el->items[j + 1] = t;
        }
    }
    apply_name_list(el, "vgm.dir", 1);
    apply_name_list(el, "FILES.LST", 0);
}

typedef struct {
    char path[MAX_QPATH];
    int machine;
} QueueEntry;

typedef struct {
    QueueEntry entries[MAX_QUEUE];
    int count;
} PlayQueue;

static void queue_recursive(PlayQueue far *q, char *rel_buf, int rel_len)
{
    struct find_t ff;
    if (_dos_findfirst("*.*", _A_SUBDIR | _A_NORMAL, &ff) == 0) {
        do {
            if (strcmp(ff.name, ".") == 0 || strcmp(ff.name, "..") == 0) continue;
            if (ff.attrib & _A_SUBDIR) {
                char new_buf[MAX_QPATH];
                int new_len;
                if (path_depth >= MAX_PATH_DEPTH) continue;
                sprintf(new_buf, "%s%s\\", rel_buf, ff.name);
                new_len = (int)strlen(new_buf);
                if (chdir(ff.name) == 0) {
                    strncpy(path_stack[path_depth], ff.name, MAX_FNAME - 1);
                    path_stack[path_depth][MAX_FNAME-1] = '\0';
                    path_depth++;
                    queue_recursive(q, new_buf, new_len);
                    path_depth--;
                    chdir("..");
                }
            } else {
                int len = (int)strlen(ff.name);
                if (len > 4 && stricmp(ff.name + len - 4, ".vgm") == 0 && q->count < MAX_QUEUE) {
                    char local_path[MAX_QPATH];
                    int k;
                    sprintf(local_path, "%s%s", rel_buf, ff.name);
                    for (k = 0; local_path[k] && k < MAX_QPATH - 1; k++)
                        q->entries[q->count].path[k] = local_path[k];
                    q->entries[q->count].path[k] = '\0';
                    q->entries[q->count].machine = detect_machine_from_stream(ff.name);
                    q->count++;
                }
            }
        } while (_dos_findnext(&ff) == 0);
    }
    (void)rel_len;
}

#define BG_TOP_LEN 4
static const unsigned char g_bg_top[BG_TOP_LEN]    = {247, 248, 249, 250};
static const unsigned char g_bg_bottom[BG_TOP_LEN] = {251, 252, 253, 254};

#define BG_SIDE_LEN 3
static const unsigned char g_bg_left_even[BG_SIDE_LEN]  = {251, 252, 253};
static const unsigned char g_bg_left_odd[BG_SIDE_LEN]   = {247, 248, 249};
static const unsigned char g_bg_right_even[BG_SIDE_LEN] = {252, 253, 254};
static const unsigned char g_bg_right_odd[BG_SIDE_LEN]  = {248, 249, 250};

#define BG_PLAY_ATTR TUI_DARKBLUE_ON_BLACK

static void draw_frame_bg(void)
{
    int row, col;
    for (col = 0; col < TUI_COLS; col++)
        tui_putc(0, col, (char)g_bg_top[col % BG_TOP_LEN], BG_PLAY_ATTR);
    for (row = 1; row <= 22; row++) {
        int even = ((row - 1) % 2) == 0;
        const unsigned char *left = even ? g_bg_left_even : g_bg_left_odd;
        const unsigned char *right = even ? g_bg_right_even : g_bg_right_odd;
        tui_putc(row, 0, (char)left[0], BG_PLAY_ATTR);
        tui_putc(row, 1, (char)left[1], BG_PLAY_ATTR);
        tui_putc(row, 2, (char)left[2], BG_PLAY_ATTR);
        tui_putc(row, 77, (char)right[0], BG_PLAY_ATTR);
        tui_putc(row, 78, (char)right[1], BG_PLAY_ATTR);
        tui_putc(row, 79, (char)right[2], BG_PLAY_ATTR);
    }
    for (col = 0; col < TUI_COLS; col++) {
        tui_putc(23, col, (char)g_bg_top[col % BG_TOP_LEN], BG_PLAY_ATTR);
        tui_putc(24, col, (char)g_bg_bottom[col % BG_TOP_LEN], BG_PLAY_ATTR);
    }
}

static void draw_frame_bg_browser(void)
{
    int row, col;
    for (col = 0; col < TUI_COLS; col++)
        tui_putc(0, col, (char)g_bg_top[col % BG_TOP_LEN], TUI_DARKBLUE_ON_BLACK);
    for (row = 1; row <= 23; row++) {
        int even = ((row - 1) % 2) == 0;
        const unsigned char *left = even ? g_bg_left_even : g_bg_left_odd;
        const unsigned char *right = even ? g_bg_right_even : g_bg_right_odd;
        tui_putc(row, 0, (char)left[0], TUI_DARKBLUE_ON_BLACK);
        tui_putc(row, 1, (char)left[1], TUI_DARKBLUE_ON_BLACK);
        tui_putc(row, 78, (char)right[0], TUI_DARKBLUE_ON_BLACK);
        tui_putc(row, 79, (char)right[1], TUI_DARKBLUE_ON_BLACK);
    }
    for (col = 0; col < TUI_COLS; col++)
        tui_putc(24, col, (char)g_bg_bottom[col % BG_TOP_LEN], TUI_DARKBLUE_ON_BLACK);
}

static void draw_browser_frame(void)
{
    int i;
    tui_clear(TUI_CYAN_ON_BLACK);
    tui_box(1, 2, 76, 23, TUI_CYAN_ON_BLACK);
    for (i = 2; i <= 22; i++)
        tui_putc(i, 77, (char)0xAF, TUI_CYAN_ON_BLACK);
    draw_frame_bg_browser();
    tui_puts(1, 4, " vgmdos ", TUI_YELLOW_ON_BLACK);
    {
        char pathline[70] = "/";
        for (i = 0; i < path_depth; i++) {
            strcat(pathline, path_stack[i]);
            strcat(pathline, "/");
        }
        tui_puts_padded(2, 4, pathline, 72, TUI_GRAY_ON_BLACK);
    }
    tui_puts(21, 4, TX("Flechas: mover   Enter: entrar en directorio / reproducir fichero",
                       "Arrows: move   Enter: open directory / play file"),
              TUI_GRAY_ON_BLACK);
    tui_puts(22, 4, TX("Espacio: reproducir directorio entero   S: FILES.LST   I: Acerca de",
                       "Space: play whole directory   S: FILES.LST   I: About"),
              TUI_GRAY_ON_BLACK);
}

static void write_files_lst(EntryList far *el)
{
    FILE *out;
    int i;
    tui_puts_padded(24, 4, TX("Creando FILES.LST...", "Creating FILES.LST..."), 70, TUI_YELLOW_ON_BLACK);
    out = fopen("FILES.LST", "w");
    if (!out) {
        tui_puts_padded(24, 4, TX("No se pudo crear FILES.LST.", "Could not create FILES.LST."), 70, TUI_RED_ON_BLACK);
        return;
    }
    for (i = 0; i < el->count; i++) {
        char namebuf[MAX_FNAME];
        char titlebuf[VGM_TAG_MAXLEN];
        if (el->items[i].is_dir) continue;
        far_name_copy(namebuf, el->items[i].name, MAX_FNAME);
        if (!el->items[i].gd3_tried) {
            VgmFile vgm;
            VgmTag tag;
            int k;
            titlebuf[0] = '\0';
            if (vgm_open(&vgm, namebuf) == 0) {
                vgm_read_gd3(&vgm, &tag);
                if (tag.has_gd3 && tag.track_name[0]) {
                    for (k = 0; k < VGM_TAG_MAXLEN - 1 && tag.track_name[k]; k++)
                        titlebuf[k] = tag.track_name[k];
                    titlebuf[k] = '\0';
                }
                vgm_close(&vgm);
            }
            for (k = 0; titlebuf[k] && k < VGM_TAG_MAXLEN - 1; k++)
                el->items[i].gd3_title[k] = titlebuf[k];
            el->items[i].gd3_title[k] = '\0';
            el->items[i].gd3_tried = 1;
        }
        far_name_copy(titlebuf, el->items[i].gd3_title, VGM_TAG_MAXLEN);
        fprintf(out, "%s=%s\r\n", namebuf, titlebuf);
    }
    fclose(out);
    tui_puts_padded(24, 4, TX("FILES.LST creado.", "FILES.LST created."), 70, TUI_YELLOW_ON_BLACK);
}

static void draw_entry_row(EntryList far *el, int idx, int row, int selected)
{
    char line[96];
    char namebuf[MAX_FNAME];
    char altbuf[MAX_ALTNAME];
    char titlebuf[VGM_TAG_MAXLEN];
    unsigned char attr = selected ? TUI_WHITE_ON_RED : TUI_CYAN_ON_BLACK;
    int i;
    if (idx == -1) {
        tui_puts(row, 4, "\x01\x02 ", TUI_YELLOW_ON_BLACK);
        tui_puts_padded(row, 7, "..", 67, attr);
        return;
    }
    for (i = 0; i < MAX_FNAME - 1 && el->items[idx].name[i]; i++)
        namebuf[i] = el->items[idx].name[i];
    namebuf[i] = '\0';
    for (i = 0; i < MAX_ALTNAME - 1 && el->items[idx].alt_name[i]; i++)
        altbuf[i] = el->items[idx].alt_name[i];
    altbuf[i] = '\0';
    if (g_noshort && !el->items[idx].is_dir && !el->items[idx].gd3_tried) {
        VgmFile vgm;
        VgmTag tag;
        int k;
        titlebuf[0] = '\0';
        if (vgm_open(&vgm, namebuf) == 0) {
            vgm_read_gd3(&vgm, &tag);
            if (tag.has_gd3 && tag.track_name[0]) {
                for (k = 0; k < VGM_TAG_MAXLEN - 1 && tag.track_name[k]; k++)
                    titlebuf[k] = tag.track_name[k];
                titlebuf[k] = '\0';
            }
            vgm_close(&vgm);
        }
        for (k = 0; titlebuf[k] && k < VGM_TAG_MAXLEN - 1; k++)
            el->items[idx].gd3_title[k] = titlebuf[k];
        el->items[idx].gd3_title[k] = '\0';
        el->items[idx].gd3_tried = 1;
    }
    for (i = 0; i < VGM_TAG_MAXLEN - 1 && el->items[idx].gd3_title[i]; i++)
        titlebuf[i] = el->items[idx].gd3_title[i];
    titlebuf[i] = '\0';
    if (el->items[idx].is_dir) {
        const char *dname = altbuf[0] ? altbuf : namebuf;
        tui_puts(row, 4, "\x01\x02 ", TUI_YELLOW_ON_BLACK);
        tui_puts_padded(row, 7, dname, 67, attr);
        return;
    }
    tui_puts(row, 4, "\x0D\x0E ", TUI_GREEN_ON_BLACK);
    if (g_noshort && titlebuf[0]) {
        char numbuf[8];
        int nlen = 0;
        while (nlen < 7 && namebuf[nlen] >= '0' && namebuf[nlen] <= '9')
            nlen++;
        if (nlen > 0) {
            for (i = 0; i < nlen; i++) numbuf[i] = namebuf[i];
            numbuf[nlen] = '\0';
        } else {
            int pos = 1, j;
            for (j = 0; j < idx; j++)
                if (!el->items[j].is_dir) pos++;
            sprintf(numbuf, "%02d", pos);
            nlen = (int)strlen(numbuf);
        }
        {
            unsigned char numattr = selected ? attr : TUI_BLUE_ON_BLACK;
            tui_puts(row, 7, numbuf, numattr);
            if (selected) {
                char rest[90];
                sprintf(rest, " - %s", titlebuf);
                tui_puts_padded(row, 7 + nlen, rest, 67 - nlen, attr);
            } else {
                char restbuf[90];
                tui_puts(row, 7 + nlen, " - ", TUI_YELLOW_ON_BLACK);
                sprintf(restbuf, "%s", titlebuf);
                tui_puts_padded(row, 7 + nlen + 3, restbuf, 67 - nlen - 3, attr);
            }
        }
        return;
    } else
        strcpy(line, namebuf);
    tui_puts_padded(row, 7, line, 67, attr);
}

static void show_about(void)
{
    static const char *es[] = {
        "Gracias por usar mi reproductor!",
        "",
        "Chips FM (por la tarjeta):",
        "  OPL2 / OPL3, MSX-Audio, YM2413 (Master System/MSX)",
        "  YM2203 / YM2608 (PC-88), Mega Drive, Neo Geo (vgmtool)",
        "Chips por software:",
        "  SN76489 (Master System, Game Gear, Tandy), NGP",
        "  AY8910 / YM2149 (MSX, ZX Spectrum, Atari ST), SCC",
        "  NES + FDS, Game Boy, WonderSwan, PC Engine, Mega CD",
        "",
        "Salida: Sound Blaster / Pro / 16 / AWE, DAC en LPT,",
        "        PicoGUS (Tandy)",
        "vgmtool (Windows/DOS): convierte y comprime los VGM",
        0 };
    static const char *en[] = {
        "Thanks for using my player!",
        "",
        "FM chips (on the sound card):",
        "  OPL2 / OPL3, MSX-Audio, YM2413 (Master System/MSX)",
        "  YM2203 / YM2608 (PC-88), Mega Drive, Neo Geo (vgmtool)",
        "Software chips:",
        "  SN76489 (Master System, Game Gear, Tandy), NGP",
        "  AY8910 / YM2149 (MSX, ZX Spectrum, Atari ST), SCC",
        "  NES + FDS, Game Boy, WonderSwan, PC Engine, Mega CD",
        "",
        "Output: Sound Blaster / Pro / 16 / AWE, LPT DAC,",
        "        PicoGUS (Tandy)",
        "vgmtool (Windows/DOS): converts and compresses VGMs",
        0 };
    const char **t = g_en ? en : es;
    int r, i;
    for (r = 4; r <= 21; r++) tui_puts_padded(r, 11, "", 58, TUI_CYAN_ON_BLACK);
    tui_box(4, 11, 58, 18, TUI_CYAN_ON_BLACK);
    tui_puts(5, 14, "VGMDOS 2026 - TheElf", TUI_YELLOW_ON_BLACK);
    for (i = 0; t[i]; i++)
        tui_puts(7 + i, 14, t[i], i == 0 ? TUI_WHITE_ON_BLACK :
                 (t[i][0] == ' ' ? TUI_GRAY_ON_BLACK : TUI_CYAN_ON_BLACK));
    tui_puts(20, 14, TX("Pulsa una tecla...", "Press any key..."), TUI_GREEN_ON_BLACK);
    tui_getkey();
}

static int sel_stack[MAX_PATH_DEPTH + 1];
static int top_stack[MAX_PATH_DEPTH + 1];

static int browse(EntryList far *el, int *out_idx)
{
    int sel, top;
    const int visible = 18;
    int total = el->count + (path_depth > 0 ? 1 : 0);
    int has_dotdot = (path_depth > 0);
    int key, i;
    int prev_sel = -999, prev_top = -999;
    sel = sel_stack[path_depth];
    top = top_stack[path_depth];
    if (sel >= total) sel = total > 0 ? total - 1 : 0;
    if (sel < 0) sel = 0;
    if (top > sel) top = sel;
    if (top < 0) top = 0;
  {
        char pathline[70] = "/";
        for (i = 0; i < path_depth; i++) {
            strcat(pathline, path_stack[i]);
            strcat(pathline, "/");
        }
        tui_puts_padded(2, 4, pathline, 72, TUI_GRAY_ON_BLACK);
    }
    for (;;) {
        if (top != prev_top || prev_sel == -999) {
            for (i = 0; i < visible; i++) {
                int pos = top + i;
                if (pos < total) {
                    int idx = has_dotdot ? pos - 1 : pos;
                    draw_entry_row(el, idx, 3 + i, pos == sel);
                } else {
                    tui_puts_padded(3 + i, 4, "", 70, TUI_CYAN_ON_BLACK);
                }
            }
        } else if (sel != prev_sel) {
            int old_row = 3 + (prev_sel - top);
            int new_row = 3 + (sel - top);
            int old_idx = has_dotdot ? prev_sel - 1 : prev_sel;
            int new_idx = has_dotdot ? sel - 1 : sel;
            if (prev_sel >= top && prev_sel < top + visible)
                draw_entry_row(el, old_idx, old_row, 0);
            if (sel >= top && sel < top + visible)
                draw_entry_row(el, new_idx, new_row, 1);
        }
        prev_sel = sel; prev_top = top;
        key = tui_getkey();
        if (key == TUI_KEY_UP) { if (sel > 0) sel--; }
        else if (key == TUI_KEY_DOWN) { if (sel < total - 1) sel++; }
        else if (key == TUI_KEY_PGUP) { sel -= visible; if (sel < 0) sel = 0; }
        else if (key == TUI_KEY_PGDN) { sel += visible; if (sel >= total) sel = total - 1; }
        else if (key == TUI_KEY_HOME) sel = 0;
        else if (key == TUI_KEY_END) sel = total - 1;
        else if (key == TUI_KEY_ESC) {
            sel_stack[path_depth] = sel; top_stack[path_depth] = top;
            return 0;
        }
        else if (key == 'i' || key == 'I') {
            show_about();
            draw_browser_frame();
            prev_sel = -999;
        }
        else if (key == 's' || key == 'S') {
            write_files_lst(el);
            prev_sel = -999;
        }
        else if (key == TUI_KEY_ENTER || key == ' ') {
            int idx = has_dotdot ? sel - 1 : sel;
            *out_idx = idx;
            sel_stack[path_depth] = sel; top_stack[path_depth] = top;
            if (idx == -1) return 1;
            if (el->items[idx].is_dir)
                return (key == ' ') ? 3 : 1;
            else
                return 2;
        }
        if (top > sel) top = sel;
        if (sel >= top + visible) top = sel - visible + 1;
        if (top != prev_top) {  }
    }
}

static const char *machine_short_names[] = {
    "Master System / Game Gear / SG-1000 / Tandy",
    "Neo Geo Pocket (T6W28)",
    "MSX (AY8910)",
    "MSX (AY8910 + SCC)",
    "NES / Famicom (APU 2A03)",
    "Game Boy / Game Boy Color (DMG)",
    "PC-88 (YM2203)",
    "WonderSwan / WonderSwan Color",
    "PC Engine / TurboGrafx-16 (HuC6280)",
    "AdLib / OPL2 (YM3812)",
    "OPL original (YM3526)",
    "MSX-Audio (Y8950)",
    "OPL3 (YMF262)",
    "AY8910 + OPL3",
    "Mega Drive / Genesis (YM2612 -> OPL3)",
    "Mega CD (RF5C164)"
};

static void draw_wrapped_text(int row, int col, const char *text, int width,
                               int max_lines, unsigned char attr)
{
    const char *p = text;
    int line = 0;
    while (line < max_lines && *p) {
        char buf[128];
        int len = 0;
        while (*p == '\r' || *p == '\n')
            p++;
        if (!*p)
            break;
        while (*p && *p != '\r' && *p != '\n' && len < width) {
            buf[len++] = *p++;
        }
        buf[len] = '\0';
        if (*p && *p != '\r' && *p != '\n' && len == width) {
            int cut = len;
            while (cut > 0 && buf[cut] != ' ')
                cut--;
            if (cut > 0) {
                buf[cut] = '\0';
                p -= (len - cut - 1);
            }
        }
        tui_puts_padded(row + line, col, buf, width, attr);
        line++;
        if (*p == '\r') p++;
        if (*p == '\n') p++;
    }
}

static void rtrim(char *s)
{
    int len = strlen(s);
    while (len > 0) {
        if (s[len - 1] == ' ' || s[len - 1] == '\t') {
            s[len - 1] = 0;
            len--;
        } else {
            break;
        }
    }
}

#define PIANO_OCTAVES 8
#define PIANO_KEYS 7
#define PIANO_ROW0 17
#define PIANO_COL0 12
#define PIANO_TOTAL_COLS (PIANO_OCTAVES * PIANO_KEYS)
#define PIANO_CLOSE_COL (PIANO_COL0 + PIANO_TOTAL_COLS)

#define PIANO_VISUAL_CHANNELS 8

static int g_piano_last_key[PIANO_VISUAL_CHANNELS];

static int g_piano_generic_col[PIANO_VISUAL_CHANNELS];
static int g_piano_generic_dirty[PIANO_VISUAL_CHANNELS];

static unsigned char g_piano_generic_next_ch = 0;

static void draw_piano_static(AllChips *c)
{
    int row, col, ch;
    (void)c;
    for (row = 0; row < 3; row++) {
        unsigned char glyph_base = (row == 0) ? 0xB0 : (row == 1) ? 0xC0 : 0xD0;
        unsigned char close_glyph = (row == 0) ? 0xB7 : (row == 1) ? 0xB8 : 0xB9;
        for (col = 0; col < PIANO_TOTAL_COLS; col++)
            tui_putc(PIANO_ROW0 + row, PIANO_COL0 + col,
                     (char)(glyph_base + (col % PIANO_KEYS)), TUI_WHITE_ON_BLACK);
        tui_putc(PIANO_ROW0 + row, PIANO_CLOSE_COL, (char)close_glyph, TUI_WHITE_ON_BLACK);
    }
    for (ch = 0; ch < PIANO_VISUAL_CHANNELS; ch++) {
        g_piano_last_key[ch] = -1;
        g_piano_generic_col[ch] = -1;
        g_piano_generic_dirty[ch] = 0;
    }
    g_piano_generic_next_ch = 0;
}

static void piano_generic_touch(unsigned char seed)
{
    static unsigned char skip = 0;
    int ch, col;
    if (++skip & 3) return;
    ch = g_piano_generic_next_ch;
    if (++g_piano_generic_next_ch >= PIANO_VISUAL_CHANNELS) g_piano_generic_next_ch = 0;
    col = seed;
    while (col >= PIANO_TOTAL_COLS) col -= PIANO_TOTAL_COLS;
    g_piano_generic_col[ch] = col;
    g_piano_generic_dirty[ch] = 1;
}

static void piano_set_key(int physical_col, unsigned char attr_top,
                           unsigned char attr_mid, unsigned char attr_bot)
{
    tui_setattr(PIANO_ROW0 + 0, PIANO_COL0 + physical_col, attr_top);
    tui_setattr(PIANO_ROW0 + 1, PIANO_COL0 + physical_col, attr_mid);
    tui_setattr(PIANO_ROW0 + 2, PIANO_COL0 + physical_col, attr_bot);
}

static void draw_piano_dynamic(AllChips *c)
{
    int ch;
    (void)c;
    for (ch = 0; ch < PIANO_VISUAL_CHANNELS; ch++) {
        int col;
        if (!g_piano_generic_dirty[ch]) continue;
        g_piano_generic_dirty[ch] = 0;
        col = g_piano_generic_col[ch];
        if (col == g_piano_last_key[ch]) continue;
        if (g_piano_last_key[ch] != -1)
            piano_set_key(g_piano_last_key[ch],
                TUI_WHITE_ON_BLACK, TUI_WHITE_ON_BLACK, TUI_WHITE_ON_BLACK);
        if (col != -1)
            piano_set_key(col, TUI_CYAN_ON_BLACK, TUI_GREEN_ON_BLACK, TUI_YELLOW_ON_BLACK);
        g_piano_last_key[ch] = col;
    }
}

static void draw_now_playing_static(const char *fname, VgmTag *tag, AllChips *c,
                                     VgmHeader *hdr, int machine_idx)
{
    tui_clear(TUI_CYAN_ON_BLACK);
    tui_box(1, 3, 74, 22, TUI_CYAN_ON_BLACK);
    {
        int r;
        for (r = 2; r <= 21; r++)
            tui_putc(r, 76, (char)0xAF, TUI_CYAN_ON_BLACK);
    }
    draw_frame_bg();
    tui_putc(1, 5, ' ', TUI_CYAN_ON_BLACK);
    tui_putc(1, 6, '\x0E', TUI_BLINK(TUI_YELLOW_ON_BLACK));
    {
        char gpart[48], tpart[48];
        int col = 7;
        int combined_len;
        gpart[0] = '\0';
        tpart[0] = '\0';
        if (tag->has_gd3 && tag->game_name[0]) {
            char tmp[44];
            strncpy(tmp, tag->game_name, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = '\0';
            rtrim(tmp);
            sprintf(gpart, " %s", tmp);
        }
        if (tag->has_gd3 && tag->track_name[0]) {
            char tmp[44];
            strncpy(tmp, tag->track_name, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = '\0';
            rtrim(tmp);
            sprintf(tpart, "%s%s ", gpart[0] ? " - " : " ", tmp);
        } else if (!gpart[0]) {
            char tmp[44];
            strncpy(tmp, fname, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = '\0';
            rtrim(tmp);
            sprintf(tpart, " %s ", tmp);
        }
        combined_len = (int)strlen(gpart) + (int)strlen(tpart);
        g_np_row_shift = 0;
        if (gpart[0] && tpart[0] && (col + combined_len) > 74) {
            char tpart2[48];
            g_np_row_shift = 1;
            tui_puts(1, col, gpart, TUI_GREEN_ON_BLACK);
            tui_puts(1, col + (int)strlen(gpart), " ", TUI_GREEN_ON_BLACK);
            strncpy(tpart2, tpart[0] == ' ' && tpart[1] == '-' ? tpart + 3 : tpart + 1, sizeof(tpart2) - 1);
            tpart2[sizeof(tpart2) - 1] = '\0';
            tui_puts(2, col + 1, tpart2, TUI_YELLOW_ON_BLACK);
        } else {
            if (gpart[0]) {
                tui_puts(1, col, gpart, TUI_GREEN_ON_BLACK);
                col += (int)strlen(gpart);
            }
            if (tpart[0])
                tui_puts(1, col, tpart, TUI_YELLOW_ON_BLACK);
        }
    }
    if (tag->has_gd3 && tag->author_name[0]) {
        char line[80];
        sprintf(line, TX("Compositor  : %s", "Composer    : %s"), tag->author_name);
        tui_puts_padded(3 + g_np_row_shift, 6, line, 70, TUI_MAGENTA_ON_BLACK);
    }
    {
        char sysline[80];
        unsigned char sys_attr;
        switch (machine_idx) {
            case MACH_NES: case MACH_GB: sys_attr = TUI_RED_ON_BLACK; break;
            case MACH_PSG: case MACH_MD: sys_attr = TUI_BLUE_ON_BLACK; break;
            case MACH_NGP: sys_attr = TUI_YELLOW_ON_BLACK; break;
            case MACH_MSX: case MACH_MSXSCC: sys_attr = TUI_GREEN_ON_BLACK; break;
            case MACH_PC88: sys_attr = TUI_MAGENTA_ON_BLACK; break;
            case MACH_WSWAN: sys_attr = TUI_GRAY_ON_BLACK; break;
            case MACH_PCE: sys_attr = TUI_YELLOW_ON_BLACK; break;
            default: sys_attr = TUI_CYAN_ON_BLACK; break;
        }
        if (tag->has_gd3 && tag->system_name[0])
            sprintf(sysline, TX("Sistema     : %s", "System      : %s"), tag->system_name);
        else
            sprintf(sysline, TX("Sistema     : %s", "System      : %s"), machine_short_names[machine_idx]);
        tui_puts_padded(4 + g_np_row_shift, 6, sysline, 70, sys_attr);
    }
    {
        char chipline[200];
        sprintf(chipline, "Chips       : %s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s",
                c->has_psg ? (c->dual_chip ? "SN76489(dual) " : "SN76489 ") : "",
                c->has_ay ? "AY8910 " : "",
                c->has_scc ? "SCC " : "",
                c->has_nes ? "NES-APU " : "",
                c->has_fds ? "+FDS " : "",
                c->has_gb ? "GB-APU " : "",
                c->has_ay2203 ? "YM2203-PSG " : "",
                c->has_ym2203fm ? "YM2203/8-FM " : "",
                c->has_ws ? "WSwan " : "",
                c->has_pce ? "PCE-PSG " : "",
                c->has_ym2413 ? "YM2413 " : "",
                c->has_ym3812 ? "YM3812(OPL2) " : "",
                c->has_ym3526 ? "YM3526(OPL) " : "",
                c->has_y8950 ? "Y8950 " : "",
                c->has_ymf262 ? "YMF262(OPL3) " : "",
                c->has_rf5c ? "RF5C164 " : (c->rf5c_nomem ? TX("RF5C164(sin memoria) ", "RF5C164(no memory) ") : ""),
                c->has_dac ? "+DAC(PCM) " : "");
        tui_puts_padded(5 + g_np_row_shift, 6, chipline, 70, TUI_CYAN_ON_BLACK);
    }
    {
        char infoline[80];
        unsigned long total_secs = vgm_estimated_total_samples(hdr, VGM_DEFAULT_LOOPS) / VGM_SAMPLE_RATE;
        sprintf(infoline, TX("VGM v%lx.%02lx  |  Duracion: %lu:%02lu", "VGM v%lx.%02lx  |  Length: %lu:%02lu"),
                (hdr->version >> 8) & 0xFF, hdr->version & 0xFF,
                total_secs / 60, total_secs % 60);
        tui_puts_padded(6 + g_np_row_shift, 6, infoline, 70, TUI_WHITE_ON_BLACK);
    }
    if (tag->has_gd3 && tag->release_date[0]) {
        char line[80];
        sprintf(line, TX("Lanzamiento : %s", "Released    : %s"), tag->release_date);
        tui_puts_padded(7 + g_np_row_shift, 6, line, 70, TUI_CYAN_ON_BLACK);
    }
    if (tag->has_gd3 && tag->vgm_by[0]) {
        char line[80];
        sprintf(line, TX("VGM por     : %s", "VGM by      : %s"), tag->vgm_by);
        tui_puts_padded(8 + g_np_row_shift, 6, line, 70, TUI_GRAY_ON_BLACK);
    }
    {
        char line[40];
        sprintf(line, "Loop        : %s", hdr->loop_offset ? TX("Si", "Yes") : "No");
        tui_puts_padded(9 + g_np_row_shift, 6, line, 40, TUI_YELLOW_ON_BLACK);
    }
    if (tag->has_gd3 && tag->notes[0]) {
        tui_puts(11 + g_np_row_shift, 6, TX("Notas :", "Notes :"), TUI_GRAY_ON_BLACK);
        draw_wrapped_text(11 + g_np_row_shift, 14, tag->notes, 62, 5, TUI_GRAY_ON_BLACK);
    }
    tui_puts(21, 5, TX("ESC: detener y volver al explorador", "ESC: stop and go back"), TUI_GRAY_ON_BLACK);
    draw_piano_static(c);
}

static void draw_now_playing_dynamic(AllChips *c, unsigned int secs,
                                      unsigned int total_secs)
{
    {
        char timebuf[40];
        if (total_secs > 0)
            sprintf(timebuf, TX(" Tiempo: %u:%02u / %u:%02u", " Time: %u:%02u / %u:%02u"), secs / 60, secs % 60,
                    total_secs / 60, total_secs % 60);
        else
            sprintf(timebuf, TX(" Tiempo: %u:%02u", " Time: %u:%02u"), secs / 60, secs % 60);
        tui_puts_padded(22, 52, timebuf, 22, TUI_BROWN_ON_BLACK);
    }
    draw_piano_dynamic(c);
}

static int check_stop_key(void)
{
    if (kbhit()) {
        int k = getch();
        if (k == TUI_KEY_ESC) return 1;
        if (k == 0 || k == 0xE0) { if (kbhit()) (void)getch(); return 0; }
    }
    return 0;
}

static void md_unconverted_notice(void)
{
    unsigned long t0 = pctimer_now();
    tui_clear(TUI_CYAN_ON_BLACK);
    tui_puts(11, 10, TX("VGM de Mega Drive sin convertir:", "Unconverted Mega Drive VGM:"), TUI_RED_ON_BLACK);
    tui_puts(13, 10, TX("convierte el directorio con vgmtool (YM2612 -> OPL3).",
                        "convert the directory with vgmtool (YM2612 -> OPL3)."), TUI_GRAY_ON_BLACK);
    while (pctimer_now() - t0 < PCTIMER_TICKS_PER_SEC * 2UL) {
        if (kbhit()) { (void)getch(); break; }
    }
}

static int play_file(const char *fname, int machine_idx)
{
    VgmFile vgm;
    VgmTag tag;
    static AllChips c;
    SbInfo sb;
    DosBuffer buf;
    Dsp dsp;
    int dsp_ok = 0;
    VgmEvent ev;
    int have_pend = 0;
    unsigned long t_start, samples_elapsed = 0;
    unsigned write_pos, buf_len;
    unsigned long gen_accum = 0;
    unsigned gen_pend = 0;
    int stopped = 0;
    int esc_counter = 0;
    unsigned long last_refresh_samples = 0;
    unsigned int elapsed_secs_counter = 0;
    unsigned int total_secs = 0;
    unsigned long sn_clock, ay_clock, nes_clock, gb_clock, ym2203_clock, ws_clock, ym2608_clock, pce_clock;
    unsigned long play_rate = g_quality;
    if (machine_idx == MACH_MD) { md_unconverted_notice(); return 0; }
    if (vgm_open(&vgm, fname) != 0) return 0;
    vgm_read_gd3(&vgm, &tag);
    memset(&c, 0, sizeof(c));
    sn_clock = vgm.header.sn76489_clock ? vgm.header.sn76489_clock : 3579545UL;
    ay_clock = vgm.header.ay8910_clock ? vgm.header.ay8910_clock : 1789750UL;
    nes_clock = vgm.header.nes_apu_clock ? vgm.header.nes_apu_clock : 1789773UL;
    gb_clock = vgm.header.gb_apu_clock ? vgm.header.gb_apu_clock : 4194304UL;
    ym2203_clock = vgm.header.ym2203_clock ? vgm.header.ym2203_clock : 3000000UL;
    ym2608_clock = vgm.header.ym2608_clock ? vgm.header.ym2608_clock : 8000000UL;
    ws_clock = vgm.header.wswan_clock ? vgm.header.wswan_clock : 3072000UL;
    pce_clock = vgm.header.pce_clock ? vgm.header.pce_clock : 3579545UL;
    if (!sb_detect(&sb) || !sb.found) {
        if (g_output_mode == 0) { vgm_close(&vgm); return 0; }
        memset(&sb, 0, sizeof(sb));
    }
    {
        const VgmHeader *h = &vgm.header;
        int use[Q_CHIPS], k;
        use[Q_PSG] = h->sn76489_clock != 0;
        use[Q_AY]  = h->ay8910_clock != 0 || h->ym2203_clock != 0;
        use[Q_SCC] = h->k051649_clock != 0;
        use[Q_NES] = h->nes_apu_clock != 0;
        use[Q_FDS] = h->nes_apu_clock != 0 && h->has_fds;
        use[Q_GB]  = h->gb_apu_clock != 0;
        use[Q_WS]  = h->wswan_clock != 0;
        use[Q_PCE] = h->pce_clock != 0;
        use[Q_MCD] = h->rf5c164_clock != 0;
        for (k = 0; k < Q_CHIPS; k++)
            if (use[k] && g_qcap[k] && g_qcap[k] < play_rate) play_rate = g_qcap[k];
    }
    if (g_output_mode == 0 && sb.found) {
        dsp_open(&dsp, sb.base_port);
        if (dsp_reset(&dsp)) { dsp_ok = 1; play_rate = dsp_real_rate(&dsp, play_rate); }
    }
    g_pgus_active = 0;
    g_dac_allowed = 0;
    g_md_boost = 0; g_dac_x2 = 0;
    dac_reset(play_rate);
    switch (machine_idx) {
        case MACH_PSG:
            c.has_psg = 1; c.dual_chip = 0;
            if (g_pgus_tandy_port) {
                g_pgus_active = 1;
                pgus_tandy_init(g_pgus_tandy_port);
            } else {
                psg_full_init(&c.psg, sn_clock, play_rate);
            }
            c.has_ym2413 = (vgm.header.ym2413_clock != 0);
            if (c.has_ym2413) {
                ym2413opl_init(sb.opl_port);
            } else if (!g_pgus_active) {
                psg_full_set_vol(362);
            }
            break;
        case MACH_NGP:
            c.has_psg = 1; c.dual_chip = 1;
            if (g_pgus_tandy_port) {
                g_pgus_active = 1;
                pgus_tandy_init(g_pgus_tandy_port);
            } else {
                psg_full_init(&c.psg, sn_clock, play_rate);
                psg_full_init(&c.psg2, sn_clock, play_rate);
                psg_full_set_vol(362);
            }
            break;
        case MACH_MSX:
            c.has_ay = 1;
            ay8910_init(&c.ay, ay_clock, play_rate,
                (vgm.header.ay8910_type == 0x10) ? AY8910_CHIP_YM2149 : AY8910_CHIP_AY);
            c.has_ym2413 = (vgm.header.ym2413_clock != 0);
            if (c.has_ym2413) {
                ym2413opl_init(sb.opl_port);
            }
            break;
        case MACH_MSXSCC:
            c.has_ay = (vgm.header.ay8910_clock != 0);
            if (c.has_ay)
                ay8910_init(&c.ay, ay_clock, play_rate,
                    (vgm.header.ay8910_type == 0x10) ? AY8910_CHIP_YM2149 : AY8910_CHIP_AY);
            c.has_scc = 1;
            scc_init(&c.scc,
                2UL * (vgm.header.k051649_clock ? vgm.header.k051649_clock : 3579545UL),
                play_rate);
            scc_set_gain(!c.has_ay);
            c.has_ym2413 = (vgm.header.ym2413_clock != 0);
            if (c.has_ym2413) {
                ym2413opl_init(sb.opl_port);
            }
            break;
        case MACH_NES:
            gbapu_free_noise_tables();
            c.has_nes = 1;
            nesapu_init(&c.nes, nes_clock, play_rate);
            g_dac_allowed = 1;
            g_nes_dmc = &c.nes;
            c.has_fds = vgm.header.has_fds;
            if (c.has_fds)
                fds_init(&c.fds, nes_clock, play_rate);
            break;
        case MACH_GB:
            nesapu_free_noise_tables();
            c.has_gb = 1;
            gbapu_init(&c.gb, gb_clock, play_rate);
            break;
        case MACH_PC88:
            c.has_ay2203 = 1;
            ay8910_init(&c.ay2203,
                        (vgm.header.ym2608_clock ? ym2608_clock : ym2203_clock) / 2UL,
                        play_rate, AY8910_CHIP_YM2149);
            c.has_ym2203fm = 1;
            ym2203opl_init(sb.opl_port,
                           vgm.header.ym2608_clock ? ym2608_clock : ym2203_clock,
                           vgm.header.ym2608_clock ? 6 : 3);
            break;
        case MACH_WSWAN:
            wsapu_init(&c.ws, ws_clock, play_rate);
            c.has_ws = 1;
            break;
        case MACH_PCE:
            c.has_pce = 1;
            pceapu_init(&c.pce, pce_clock, play_rate);
            g_dac_allowed = 1;
            g_pce_dac = &c.pce;
            break;
        case MACH_OPL2:
            c.has_ym3812 = 1;
            ym3812opl_init(sb.opl_port);
            break;
        case MACH_OPL1:
            c.has_ym3526 = 1;
            ym3812opl_init(sb.opl_port);
            break;
        case MACH_MSXAUDIO:
            c.has_y8950 = 1;
            ym3812opl_init(sb.opl_port);
            break;
        case MACH_OPL3:
            g_md_boost = (vgm.header.sn_vol256 >= 0x80 && vgm.header.sn_vol256 < 0x100) ||
                         (vgm.header.dac_vol256 >= 0xC0 && vgm.header.dac_vol256 < 0x100);
            g_md_mixer = (g_output_mode == 0 && (sb.dma16 > 0 || sb.type >= 6));
            g_dac_x2 = g_md_boost && !g_md_mixer;
            c.has_psg = (vgm.header.sn76489_clock != 0); c.dual_chip = 0;
            if (c.has_psg) {
                if (g_pgus_tandy_port) {
                    g_pgus_active = 1;
                    pgus_tandy_init(g_pgus_tandy_port);
                } else {
                    psg_full_init(&c.psg, sn_clock, play_rate);
                }
                if (!g_pgus_tandy_port) {
                    unsigned v = vgm.header.sn_vol256;
                    if (g_md_boost && g_md_mixer) v = (v * 181U) >> 8;
                    psg_full_set_vol(PSG_DOS_BOOST(v));
                }
            }
            c.has_ymf262 = 1;
            c.has_dac = (vgm.header.ym2612_clock != 0);
            g_dac_allowed = c.has_dac;
            ym3812opl_init(sb.opl_port ? sb.opl_port : 0x388);
            break;
        case MACH_MCD:
            c.has_psg = (vgm.header.sn76489_clock != 0); c.dual_chip = 0;
            if (c.has_psg) psg_full_init(&c.psg, sn_clock, play_rate);
            c.has_ay = (vgm.header.ay8910_clock != 0);
            if (c.has_ay)
                ay8910_init(&c.ay, ay_clock, play_rate,
                    (vgm.header.ay8910_type == 0x10) ? AY8910_CHIP_YM2149 : AY8910_CHIP_AY);
            break;
        case MACH_AY_OPL3:
            c.has_ay = 1;
            ay8910_init(&c.ay, ay_clock, play_rate,
                (vgm.header.ay8910_type == 0x10) ? AY8910_CHIP_YM2149 : AY8910_CHIP_AY);
            c.has_ymf262 = 1;
            ym3812opl_init(sb.opl_port);
            break;
        default:
            break;
    }
    c.has_rf5c = 0; c.rf5c_nomem = 0; g_min_gen = 1;
    if (vgm.header.rf5c164_clock &&
        (machine_idx == MACH_MCD || machine_idx == MACH_OPL3 || machine_idx == MACH_AY_OPL3)) {
        c.has_rf5c = rf5c_init(vgm.header.rf5c164_clock, play_rate);
        c.rf5c_nomem = !c.has_rf5c;
        if (c.has_rf5c && vgm.header.rf5c164_clock == 12500001UL) { rf5c_set_boost(1); g_min_gen = 256; awe_try_enable(); }
        else if (c.has_rf5c) rf5c_set_half(1);
    }
    g_psg_half = 0; g_ph_have = 0;
    vgm.skip_pcm = 1;
    if (g_pgus_active) {
        buf_len = (unsigned)(play_rate / 20UL);
    } else {
        buf_len = (unsigned)(play_rate * 8UL / 10UL);
    }
    g_out16 = (g_bits16 && g_output_mode == 0 && !g_pgus_active && sb.dma16 >= 5 &&
               (!dsp_ok || dsp.ver_major >= 4) &&
               c.has_rf5c);
    if (g_out16 && buf_len > 30000U) buf_len = 30000U;
    if (!dosmem_alloc(&buf, g_out16 ? buf_len * 2U : buf_len)) {
        vgm_close(&vgm); return 0;
    }
    {
        int guard = 0;
        g_pre_n = 0;
        g_oplq_lat = 0;
        while (guard++ < 4000 && vgm_next_event(&vgm, &ev) == 0) {
            if (ev.type == VGM_EV_WAIT) { have_pend = 1; break; }
            if (!c.has_rf5c && g_pre_n < PRE_MAX &&
                ev.type != VGM_EV_PCM_BANK && ev.type != VGM_EV_DAC_TABLE &&
                ev.type != VGM_EV_NES_RAM && ev.type != VGM_EV_NES_PCM) {
                g_pre[g_pre_n++] = ev;
            }
            else if (ev.type >= VGM_EV_PCM_BANK && ev.type <= VGM_EV_DAC_STOP) dac_event(&vgm, &ev);
            else if (ev.type >= VGM_EV_RF5C_WRITE && ev.type <= VGM_EV_RF5C_TABLE) { rf5c_event(&vgm, &ev); awe_forward(&c, &ev, 0); }
            else route_write(&c, ev.type, ev.addr, ev.data, 0);
        }
    }
    write_pos = 0;
    mix_generate(&c, buf.ptr, buf_len, &write_pos, buf_len);
    write_pos = 0;
    {
        unsigned k, nr = (unsigned)(play_rate / 50UL);
        if (nr > buf_len) nr = buf_len;
        if (g_out16) {
            int far *b16 = (int far *)buf.ptr;
            for (k = 0; k < nr; k++) b16[k] = (int)(((long)b16[k] * (long)k) / (long)nr);
        } else
        for (k = 0; k < nr; k++)
            buf.ptr[k] = (unsigned char)(128 + (((int)buf.ptr[k] - 128) * (long)k) / (long)nr);
    }
    g_swaudio_active = 0;
    if (g_output_mode == 0) {
        if (!dsp_ok) dsp_open(&dsp, sb.base_port);
        if (!dsp_ok && !dsp_reset(&dsp)) {
            dosmem_free(&buf); vgm_close(&vgm); return 0;
        }
        g_mix_msx = ((machine_idx == MACH_MSX || machine_idx == MACH_MSXSCC) && c.has_ym2413);
        g_mix_pcm = c.has_rf5c;
        g_mix_solo = !(c.has_ym2413 || c.has_ym2203fm || c.has_ym3812 || c.has_ym3526 ||
                       c.has_y8950 || c.has_ymf262);
        sbmix_apply(sb.base_port, (sb.dma16 > 0 || sb.type >= 6));
        if (!irq_install(sb.irq, sb.base_port)) {
            dosmem_free(&buf); vgm_close(&vgm); return 0;
        }
        if (g_out16 && dsp.ver_major < 4) {
            g_out16 = 0;
            _fmemset(buf.ptr, 128, buf_len);
        }
        g_dma_ch = g_out16 ? sb.dma16 : sb.dma8;
        if (g_out16) dsp_play_loop16(&dsp, buf.linear_addr, buf_len, play_rate, sb.dma16);
        else dsp_play_loop(&dsp, buf.linear_addr, buf_len, buf_len, play_rate, sb.dma8);
    } else {
        unsigned lpt = swaudio_detect_lpt((int)g_output_mode);
        if (lpt)
            g_swaudio_active = swaudio_start(g_output_mode, play_rate, buf.ptr, buf_len, lpt);
    }
    {
        unsigned long settle_start = pctimer_now();
        unsigned long settle_target = settle_start + (PCTIMER_TICKS_PER_SEC * 300UL / 1000UL);
        while ((long)(pctimer_now() - settle_target) < 0) ;
    }
    draw_now_playing_static(fname, &tag, &c, &vgm.header, machine_idx);
    total_secs = (unsigned int)(vgm_estimated_total_samples(&vgm.header, VGM_DEFAULT_LOOPS) / VGM_SAMPLE_RATE);
    draw_now_playing_dynamic(&c, 0, total_secs);
    g_oplq_head = g_oplq_tail = 0;
    g_oplq_lat = 0;
    if (g_output_mode == 0 &&
        (c.has_ymf262 || c.has_ym3812 || c.has_ym3526 || c.has_y8950 ||
         c.has_ym2413 || c.has_ym2203fm || g_pgus_active || rf5c_awe_on())) {
        unsigned cnt = dma_get_count(g_dma_ch);
        unsigned dpos = (cnt < buf_len) ? (buf_len - 1u - cnt) : 0u;
        unsigned ahead = (unsigned)((write_pos + buf_len - dpos) % buf_len);
        g_oplq_lat = ((unsigned long)ahead * 1000UL / play_rate) * 1193UL;
    }
    t_start = pctimer_now();
    {
        unsigned k;
        for (k = 0; k < g_pre_n; k++) {
            VgmEvent pe = g_pre[k];
            if (pe.type >= VGM_EV_PCM_BANK && pe.type <= VGM_EV_DAC_STOP) dac_event(&vgm, &pe);
            else if (g_oplq_lat && is_hw_write(&c, pe.type, pe.addr)) oplq_push(&c, pe.type, pe.addr, pe.data, t_start + g_oplq_lat);
            else route_write(&c, pe.type, pe.addr, pe.data, 0);
        }
        g_pre_n = 0;
    }
    {
    unsigned long target = t_start;
    unsigned long pit_accum = 0;
    unsigned int sync_skip = 0;
    while (!stopped && (have_pend ? (have_pend = 0, 1) : vgm_next_event(&vgm, &ev) == 0)) {
        if (++esc_counter >= 32) {
            esc_counter = 0;
            if (check_stop_key()) { stopped = 1; break; }
        }
        if (samples_elapsed - last_refresh_samples >= VGM_SAMPLE_RATE) {
            last_refresh_samples = samples_elapsed;
            elapsed_secs_counter++;
            draw_now_playing_dynamic(&c, elapsed_secs_counter, total_secs);
        }
        if (ev.type == VGM_EV_WAIT) {
            unsigned long remaining_vgm = ev.wait;
            while (remaining_vgm > 0) {
                unsigned long chunk_vgm = remaining_vgm > MAX_CHUNK_VGM ? MAX_CHUNK_VGM : remaining_vgm;
                unsigned long numerator;
                unsigned int play_samples, rem16;
                numerator = fast_mul16((unsigned int)chunk_vgm, (unsigned int)play_rate) + gen_accum;
                play_samples = fast_div32_16(numerator, (unsigned int)VGM_SAMPLE_RATE, &rem16);
                gen_accum = rem16;
                gen_pend += play_samples;
                if (gen_pend >= g_min_gen) {
                    mix_generate(&c, buf.ptr, buf_len, &write_pos, gen_pend);
                    if (g_swaudio_active) swaudio_set_write_pos(write_pos);
                    gen_pend = 0;
                }
                samples_elapsed += chunk_vgm;
                {
                    unsigned long pit_num = chunk_vgm * PCTIMER_TICKS_PER_SEC + pit_accum;
                    unsigned int delta_ticks;
                    delta_ticks = fast_div32_16(pit_num, (unsigned int)VGM_SAMPLE_RATE, &rem16);
                    target += delta_ticks;
                    pit_accum = rem16;
                }
                remaining_vgm -= chunk_vgm;
                if (++sync_skip >= 8 || remaining_vgm == 0) {
                    unsigned long now;
                    while ((long)((now = pctimer_now()) - target) < 0) {
                        if (g_oplq_head != g_oplq_tail) oplq_service(&c, now);
                    }
                    if (g_oplq_head != g_oplq_tail) oplq_service(&c, now);
                    sync_skip = 0;
                }
            }
        } else if (ev.type >= VGM_EV_PCM_BANK && ev.type <= VGM_EV_DAC_STOP) {
            dac_event(&vgm, &ev);
        } else if (ev.type >= VGM_EV_RF5C_WRITE && ev.type <= VGM_EV_RF5C_TABLE) {
            if (c.has_rf5c) { rf5c_event(&vgm, &ev); awe_forward(&c, &ev, target + g_oplq_lat); }
        } else if (g_oplq_lat && is_hw_write(&c, ev.type, ev.addr)) {
            oplq_push(&c, ev.type, ev.addr, ev.data, target + g_oplq_lat);
        } else {
            route_write(&c, ev.type, ev.addr, ev.data, samples_elapsed);
        }
    }
    if (!stopped && g_oplq_head != g_oplq_tail) {
        if (g_output_mode == 0) {
            unsigned cnt = dma_get_count(g_dma_ch);
            unsigned dpos = (cnt < buf_len) ? (buf_len - 1u - cnt) : 0u;
            unsigned stale = (unsigned)((dpos + buf_len - write_pos) % buf_len);
            unsigned k, w = write_pos;
            if (stale > 32u) stale -= 32u; else stale = 0;
            for (k = 0; k < stale; k++) {
                if (g_out16) ((int far *)buf.ptr)[w] = 0;
                else buf.ptr[w] = 128;
                if (++w >= buf_len) w = 0;
            }
        }
        while (g_oplq_head != g_oplq_tail) {
            if (check_stop_key()) { stopped = 1; break; }
            oplq_service(&c, pctimer_now());
        }
    }
    g_oplq_head = g_oplq_tail = 0;
    }
    if (c.has_ym2413 || c.has_ym3812 || c.has_ym3526 || c.has_y8950 ||
        c.has_ymf262 || c.has_ym2203fm)
        opl_hard_silence(c.has_ymf262 || c.has_ym2203fm);
    if (c.has_ym2413)
        ym2413opl_silence();
    if (c.has_ym3812 || c.has_ym3526 || c.has_y8950)
        ym3812opl_silence();
    if (c.has_ymf262)
        ymf262opl_silence();
    if (c.has_ym2203fm)
        ym2203opl_silence();
    if (g_pgus_active)
        pgus_tandy_silence();
    if (g_swaudio_active) { swaudio_stop(); g_swaudio_active = 0; }
    if (g_output_mode == 0) { dsp_stop(&dsp); irq_remove(); }
    dac_free();
    if (c.has_rf5c) rf5c_free();
    dosmem_free(&buf);
    vgm_close(&vgm);
    draw_browser_frame();
    return stopped;
}

static void play_folder_queue(void)
{
    static PlayQueue far q;
    int i;
    q.count = 0;
    queue_recursive(&q, "", 0);
    for (i = 0; i < q.count; i++) {
        char local_path[MAX_QPATH];
        int k;
        for (k = 0; q.entries[i].path[k] && k < MAX_QPATH - 1; k++)
            local_path[k] = q.entries[i].path[k];
        local_path[k] = '\0';
        if (play_file(local_path, q.entries[i].machine))
            break;
    }
}

static int chdir_drive(const char *path)
{
    if (path[0] && path[1] == ':') {
        unsigned want = (unsigned)(toupper((unsigned char)path[0]) - 'A' + 1), ndrv, got;
        _dos_setdrive(want, &ndrv);
        _dos_getdrive(&got);
        if (got != want) return -1;
        if (path[2] == '\0') return 0;
    }
    return chdir(path);
}

static char g_pgus_path[80];
static char g_pgus_drive = 0;
static int g_pgus_last = -1;
static char jump_back[MAX_PATH_DEPTH][80];

static void trim_line(char *l)
{
    int n = (int)strlen(l);
    while (n > 0 && (l[n-1] == '\n' || l[n-1] == '\r' || l[n-1] == ' ' || l[n-1] == '\t')) l[--n] = '\0';
}

static void pgus_window(const char *l1, const char *l2)
{
    int r;
    for (r = 9; r < 15; r++) tui_puts_padded(r, 14, "", 52, TUI_WHITE_ON_BLACK);
    tui_box(9, 14, 52, 6, TUI_CYAN_ON_BLACK);
    tui_puts(9, 16, " PicoGUS ", TUI_YELLOW_ON_BLACK);
    tui_puts(11, 17, l1, TUI_WHITE_ON_BLACK);
    if (l2) tui_puts(13, 17, l2, TUI_GRAY_ON_BLACK);
}

static void pgus_hidecursor(void)
{
    union REGS r;
    r.h.ah = 0x01; r.x.cx = 0x2000; int86(0x10, &r, &r);
}

static void pgus_msg(const char *es, const char *en)
{
    pgus_window(TX(es, en), TX("Pulsa una tecla para continuar...", "Press a key to continue..."));
    pgus_hidecursor();
    tui_getkey();
    draw_browser_frame();
}

static int pgus_try_jump(void)
{
    FILE *f;
    char l1[40], l2[64], root[96], cmd[200];
    int num, k;
    union REGS r;
    f = fopen("picogus.dir", "r");
    if (!f) return 0;
    l1[0] = l2[0] = '\0';
    if (fgets(l1, sizeof(l1), f)) { if (!fgets(l2, sizeof(l2), f)) l2[0] = '\0'; }
    fclose(f);
    trim_line(l1); trim_line(l2);
    num = atoi(l1);
    if (num <= 0) { pgus_msg("PICOGUS.DIR: falta el numero de CD en la primera linea.", "PICOGUS.DIR: the CD number is missing on the first line."); return 0; }
    if (!g_pgus_drive) { pgus_msg("Falta PGUSDRIVE= en el vgmdos.ini.", "PGUSDRIVE= is missing in vgmdos.ini."); return 0; }
    if (!getcwd(jump_back[path_depth - 1], sizeof(jump_back[0]))) { jump_back[path_depth - 1][0] = '\0'; return 0; }
    if (num != g_pgus_last) {
        char nb[80];
        sprintf(nb, "%s%d...", TX("Cargando CD de PicoGUS numero ", "Loading PicoGUS CD number "), num);
        pgus_window(nb, 0);
        pgus_hidecursor();
        if (g_pgus_path[0]) {
            int n = (int)strlen(g_pgus_path);
            sprintf(cmd, "%s%sPGUSINIT.EXE /cdload %d >NUL", g_pgus_path, (g_pgus_path[n-1] == '\\' ? "" : "\\"), num);
        } else sprintf(cmd, "PGUSINIT.EXE /cdload %d >NUL", num);
        g_pgus_last = -1;
        if (system(cmd) != 0) {
            jump_back[path_depth - 1][0] = '\0';
            pgus_msg("No se pudo ejecutar PGUSINIT.EXE (mira PGUSPATH= en el ini).", "Could not run PGUSINIT.EXE (check PGUSPATH= in the ini).");
            return 0;
        }
        g_pgus_last = num;
        delay(1000);
    }
    sprintf(root, "%c:\\%s", g_pgus_drive, l2);
    for (k = 0; k < 30; k++) {
        if (chdir_drive(root) == 0) break;
        delay(200);
    }
    if (k >= 30) {
        chdir_drive(jump_back[path_depth - 1]);
        jump_back[path_depth - 1][0] = '\0';
        pgus_msg("No se pudo leer la unidad del CD de PicoGUS.", "Could not read the PicoGUS CD drive.");
        return 0;
    }
    pgus_hidecursor();
    draw_browser_frame();
    return 1;
}

static void leave_dir(void)
{
    if (jump_back[path_depth][0]) {
        chdir_drive(jump_back[path_depth]);
        jump_back[path_depth][0] = '\0';
    }
    chdir("..");
}

static unsigned long quality_value(long val)
{
    if (val == 8) return 8000UL;
    if (val == 11) return 11025UL;
    if (val == 16) return 16000UL;
    if (val == 22) return 22050UL;
    if (val == 44) return 44100UL;
    if (val >= 4000 && val <= 44100) return (unsigned long)val;
    return 0;
}

static void read_config(char *out_path, int max_path, unsigned long *out_quality,
                         int *out_noshort)
{
    FILE *f;
    char line[128];
    out_path[0] = '\0';
    *out_quality = PLAY_SAMPLE_RATE;
    *out_noshort = 0;
    f = fopen("vgmdos.ini", "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' || line[len-1] == ' '))
            line[--len] = '\0';
        if (strnicmp(line, "PATH=", 5) == 0) {
            strncpy(out_path, line + 5, max_path - 1);
            out_path[max_path - 1] = '\0';
        } else if (strnicmp(line, "LANGUAGE=", 9) == 0) {
            g_en = (strnicmp(&line[9], "ES", 2) != 0);
        } else if (strnicmp(line, "BITS16=", 7) == 0) {
            g_bits16 = (line[7] == '1');
        } else if (strnicmp(line, "NOSHORT=", 8) == 0) {
            *out_noshort = (line[8] == '1') ? 1 : 0;
        } else if (strnicmp(line, "OUTPUT=", 7) == 0) {
            if (strnicmp(&line[7], "LPT1", 4) == 0) g_output_mode = SWAUDIO_LPT1;
            else if (strnicmp(&line[7], "LPT2", 4) == 0) g_output_mode = SWAUDIO_LPT2;
            else if (strnicmp(&line[7], "LPT3", 4) == 0) g_output_mode = SWAUDIO_LPT3;
            else if (strnicmp(&line[7], "LPT4", 4) == 0) g_output_mode = SWAUDIO_LPT4;
            else g_output_mode = 0;
        } else if (strnicmp(line, "PGUSPATH=", 9) == 0 || strnicmp(line, "PGUSPTAH=", 9) == 0) {
            strncpy(g_pgus_path, line + 9, sizeof(g_pgus_path) - 1);
            g_pgus_path[sizeof(g_pgus_path) - 1] = '\0';
        } else if (strnicmp(line, "PGUSDRIVE=", 10) == 0) {
            g_pgus_drive = (char)toupper((unsigned char)line[10]);
            if (g_pgus_drive < 'A' || g_pgus_drive > 'Z') g_pgus_drive = 0;
        } else if (strnicmp(line, "PGUS_TNDY=", 10) == 0) {
            if (line[10] == '1' && (line[11] == '\0' || line[11] == '\r' || line[11] == '\n')) {
                g_pgus_tandy_port = 0xC0;
            } else if (line[10] != '0') {
                g_pgus_tandy_port = (unsigned)strtoul(&line[10], 0, 16);
            }
        } else if (strnicmp(line, "QUALITY_", 8) == 0) {
            static const char *qn[Q_CHIPS] = { "PSG", "AY", "SCC", "NES", "FDS", "GB", "WS", "PCE", "MCD" };
            int k;
            for (k = 0; k < Q_CHIPS; k++) {
                int L = (int)strlen(qn[k]);
                if (strnicmp(line + 8, qn[k], L) == 0 && line[8 + L] == '=') {
                    g_qcap[k] = quality_value(atol(line + 9 + L));
                    break;
                }
            }
        } else if (strnicmp(line, "QUALITY=", 8) == 0) {
            unsigned long q = quality_value(atol(line + 8));
            if (q) *out_quality = q;
        }
    }
    fclose(f);
}

static void far_name_copy(char *dst, const char far *src, int maxlen)
{
    int i;
    for (i = 0; i < maxlen - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static int __far hard_fail(unsigned deverr, unsigned errcode, unsigned __far *devhdr)
{
    (void)deverr; (void)errcode; (void)devhdr;
    return _HARDERR_FAIL;
}

int main(void)
{
    static EntryList far el;
    char cfg_path[80];
    static char start_dir[80];
    union REGS mode_regs;
    _harderr(hard_fail);
    mode_regs.h.ah = 0x00;
    mode_regs.h.al = 0x03;
    int86(0x10, &mode_regs, &mode_regs);
    load_custom_font();
    mode_regs.h.ah = 0x01;
    mode_regs.x.cx = 0x2000;
    int86(0x10, &mode_regs, &mode_regs);
    draw_browser_frame();
    if (!getcwd(start_dir, sizeof(start_dir))) start_dir[0] = '\0';
    read_config(cfg_path, sizeof(cfg_path), &g_quality, &g_noshort);
    draw_browser_frame();
    if (g_output_mode != 0) {
        g_pgus_tandy_port = 0;
    }
    if (cfg_path[0] != '\0') {
        if (chdir_drive(cfg_path) != 0) {
            if (start_dir[0] != '\0') chdir_drive(start_dir);
            tui_clear(TUI_CYAN_ON_BLACK);
            tui_puts(12, 15, TX("No se pudo entrar en el PATH del vgmdos.ini.", "Could not open the PATH set in vgmdos.ini."), TUI_RED_ON_BLACK);
            tui_puts(14, 15, TX("Pulsa una tecla para continuar aqui...", "Press a key to continue here..."), TUI_GRAY_ON_BLACK);
            tui_getkey();
            draw_browser_frame();
        }
    }
    scan_entries(&el);
    for (;;) {
        int action, idx;
        char namebuf[MAX_FNAME];
        action = browse(&el, &idx);
        if (action == 0) break;
        if (idx >= 0) far_name_copy(namebuf, el.items[idx].name, MAX_FNAME);
        if (action == 1) {
            if (idx == -1) {
                if (path_depth > 0) {
                    path_depth--;
                    leave_dir();
                    scan_entries(&el);
                }
            } else {
                if (chdir(namebuf) == 0 && path_depth < MAX_PATH_DEPTH) {
                    strncpy(path_stack[path_depth], namebuf, MAX_FNAME - 1);
                    path_stack[path_depth][MAX_FNAME - 1] = '\0';
                    jump_back[path_depth][0] = '\0';
                    path_depth++;
                    pgus_try_jump();
                    scan_entries(&el);
                } else {
                    tui_clear(TUI_CYAN_ON_BLACK);
                    tui_puts(12, 15, TX("No se pudo entrar en ese directorio.", "Could not open that directory."), TUI_RED_ON_BLACK);
                    tui_puts(14, 15, TX("Pulsa una tecla para continuar...", "Press a key to continue..."), TUI_GRAY_ON_BLACK);
                    tui_getkey();
                    draw_browser_frame();
                }
            }
        } else if (action == 2) {
            int machine = detect_machine(namebuf);
            play_file(namebuf, machine);
            draw_browser_frame();
        } else if (action == 3) {
            if (chdir(namebuf) == 0 && path_depth < MAX_PATH_DEPTH) {
                strncpy(path_stack[path_depth], namebuf, MAX_FNAME - 1);
                path_stack[path_depth][MAX_FNAME - 1] = '\0';
                jump_back[path_depth][0] = '\0';
                path_depth++;
                pgus_try_jump();
                play_folder_queue();
                path_depth--;
                leave_dir();
                draw_browser_frame();
            } else {
                tui_clear(TUI_CYAN_ON_BLACK);
                tui_puts(12, 15, TX("No se pudo entrar en ese directorio.", "Could not open that directory."), TUI_RED_ON_BLACK);
                tui_puts(14, 15, TX("Pulsa una tecla para continuar...", "Press a key to continue..."), TUI_GRAY_ON_BLACK);
                tui_getkey();
                draw_browser_frame();
            }
        }
    }
    if (start_dir[0] != '\0') chdir_drive(start_dir);
    sbmix_restore();
    restore_default_font();
    tui_clear(TUI_WHITE_ON_BLACK);
    return 0;
}
