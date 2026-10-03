#include <string.h>
#include <stdlib.h>
#include "vgm.h"

static u32 read_u32le(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

#define VGM_LOOP_EXTEND_CAP_SAMPLES (600UL * 44100UL)

unsigned long vgm_estimated_total_samples(const VgmHeader *hdr, int max_loops)
{
    if (hdr->loop_offset != 0 && hdr->loop_samples != 0 &&
        hdr->loop_samples < VGM_LOOP_EXTEND_CAP_SAMPLES)
        return (unsigned long)hdr->total_samples + (unsigned long)hdr->loop_samples * (unsigned long)max_loops;
    return (unsigned long)hdr->total_samples;
}

static unsigned char far lz_win[4096];
static unsigned char far lz_ck_win[4096];

static unsigned char lzi[1024];
static unsigned lzi_pos = 0, lzi_len = 0;

static int lz_fill(VgmFile *v)
{
    lzi_len = (unsigned)fread(lzi, 1, sizeof(lzi), v->fp);
    lzi_pos = 0;
    if (lzi_len == 0) return EOF;
    return lzi[lzi_pos++];
}
#define LZ_IN(v) (lzi_pos < lzi_len ? (int)lzi[lzi_pos++] : lz_fill(v))

static void lz_reset(VgmFile *v)
{
    v->lz_out = v->data_start;
    v->lz_flags = 0;
    v->lz_mlen = 0;
    v->lz_mdist = 0;
    v->lz_r = 0;
    v->lz_ck_ok = 0;
    lzi_pos = lzi_len = 0;
}

static unsigned lz_decode(VgmFile *v, unsigned char *dst, unsigned n)
{
    unsigned char far *w = lz_win;
    unsigned char *d = dst, *e = dst + n;
    unsigned r = v->lz_r;
    unsigned flags = v->lz_flags;
    while (d < e) {
        int b;
        if (v->lz_mlen) {
            unsigned k = v->lz_mlen, s = (r - v->lz_mdist) & 4095;
            if (k > (unsigned)(e - d)) k = (unsigned)(e - d);
            v->lz_mlen -= k;
            do {
                unsigned char c = w[s];
                s = (s + 1) & 4095;
                w[r] = c;
                r = (r + 1) & 4095;
                *d++ = c;
            } while (--k);
            continue;
        }
        if (flags <= 1) {
            b = LZ_IN(v);
            if (b == EOF) break;
            flags = (unsigned)b | 0x100;
        }
        if (flags & 1) {
            flags >>= 1;
            b = LZ_IN(v);
            if (b == EOF) break;
            w[r] = (unsigned char)b;
            r = (r + 1) & 4095;
            *d++ = (unsigned char)b;
        } else {
            int b0, b1;
            flags >>= 1;
            b0 = LZ_IN(v);
            b1 = LZ_IN(v);
            if (b1 == EOF) break;
            v->lz_mdist = (((unsigned)b1 & 0xF0) << 4 | (unsigned)b0) + 1;
            v->lz_mlen = ((unsigned)b1 & 0x0F) + 3;
        }
    }
    v->lz_flags = flags;
    v->lz_r = r;
    v->lz_out += (long)(d - dst);
    return (unsigned)(d - dst);
}

static void lz_save(VgmFile *v)
{
    _fmemcpy(lz_ck_win, lz_win, sizeof(lz_win));
    v->lz_ck_file = ftell(v->fp) - (long)(lzi_len - lzi_pos);
    v->lz_ck_flags = v->lz_flags;
    v->lz_ck_mlen = v->lz_mlen;
    v->lz_ck_mdist = v->lz_mdist;
    v->lz_ck_r = v->lz_r;
    v->lz_ck_ok = 1;
}

static int lz_restore(VgmFile *v)
{
    if (fseek(v->fp, v->lz_ck_file, SEEK_SET) != 0) return -1;
    lzi_pos = lzi_len = 0;
    _fmemcpy(lz_win, lz_ck_win, sizeof(lz_win));
    v->lz_flags = v->lz_ck_flags;
    v->lz_mlen = v->lz_ck_mlen;
    v->lz_mdist = v->lz_ck_mdist;
    v->lz_r = v->lz_ck_r;
    v->lz_out = v->loop_start;
    return 0;
}

int vgm_open(VgmFile *vgm, const char *path)
{
    u8 hdr[0x40];
    size_t got;
    memset(vgm, 0, sizeof(*vgm));
    vgm->pcm_bank_offset = -1;
    vgm->vdac_offset = -1;
    vgm->fp = fopen(path, "rb");
    if (vgm->fp) {
        setvbuf(vgm->fp, NULL, _IOFBF, 32768);
    }
    if (!vgm->fp) {
        fprintf(stderr, "vgm_open: cannot open '%s'\n", path);
        return 1;
    }
    vgm->pcm_fp = fopen(path, "rb");
    got = fread(hdr, 1, sizeof(hdr), vgm->fp);
    if (got < 0x40) {
        fprintf(stderr, "vgm_open: file too small (incomplete header)\n");
        fclose(vgm->fp);
        return 2;
    }
    vgm->lz = (memcmp(hdr, "VgmL", 4) == 0);
    if (!vgm->lz && memcmp(hdr, "Vgm ", 4) != 0) {
        fprintf(stderr, "vgm_open: not a valid VGM file\n");
        fclose(vgm->fp);
        return 3;
    }
    vgm->header.eof_offset    = read_u32le(&hdr[0x04]);
    vgm->header.version       = read_u32le(&hdr[0x08]);
    vgm->header.sn76489_clock = read_u32le(&hdr[0x0C]);
    vgm->header.ym2413_clock  = read_u32le(&hdr[0x10]) & 0x3FFFFFFFUL;
    vgm->header.gd3_offset    = read_u32le(&hdr[0x14]);
    vgm->header.total_samples = read_u32le(&hdr[0x18]);
    vgm->header.loop_offset   = read_u32le(&hdr[0x1C]);
    vgm->header.loop_samples  = read_u32le(&hdr[0x20]);
    if (vgm->header.version >= 0x110)
        vgm->header.ym2612_clock = read_u32le(&hdr[0x2C]) & 0x3FFFFFFFUL;
    else
        vgm->header.ym2612_clock = vgm->header.ym2413_clock;
    vgm->header.sn76489_dual_chip = (vgm->header.sn76489_clock & 0x40000000UL) ? 1 : 0;
    vgm->header.sn76489_clock &= 0x3FFFFFFFUL;
    vgm->header.ay8910_clock = 0;
    vgm->header.ay8910_type = 0;
    if (vgm->header.version >= 0x151) {
        u8 extra[4];
        long cur = ftell(vgm->fp);
        if (fseek(vgm->fp, 0x74, SEEK_SET) == 0 && fread(extra, 1, 4, vgm->fp) == 4)
            vgm->header.ay8910_clock = read_u32le(extra) & 0x3FFFFFFFUL;
        if (fseek(vgm->fp, 0x78, SEEK_SET) == 0 && fread(extra, 1, 1, vgm->fp) == 1)
            vgm->header.ay8910_type = extra[0];
        fseek(vgm->fp, cur, SEEK_SET);
    }
    vgm->header.sn_vol256 = 256;
    vgm->header.dac_vol256 = 256;
    if (vgm->header.version >= 0x170) {
        u8 b[12];
        long cur = ftell(vgm->fp);
        if (fseek(vgm->fp, 0xBC, SEEK_SET) == 0 && fread(b, 1, 4, vgm->fp) == 4) {
            u32 rel = read_u32le(b);
            if (rel) {
                long ext = 0xBCL + (long)rel;
                if (fseek(vgm->fp, ext, SEEK_SET) == 0 && fread(b, 1, 12, vgm->fp) == 12) {
                    u32 size = read_u32le(&b[0]);
                    u32 voff = read_u32le(&b[8]);
                    if (size >= 12 && voff &&
                        fseek(vgm->fp, ext + 8 + (long)voff, SEEK_SET) == 0 &&
                        fread(b, 1, 1, vgm->fp) == 1) {
                        int n = b[0];
                        while (n-- > 0 && fread(b, 1, 4, vgm->fp) == 4) {
                            unsigned v = (unsigned)b[2] | ((unsigned)b[3] << 8);
                            if (b[0] == 0x00 && (v & 0x8000u)) {
                                v &= 0x7FFFu;
                                vgm->header.sn_vol256 = (v > 1024) ? 1024 : v;
                            }
                            if (b[0] == 0x02 && (v & 0x8000u)) {
                                v &= 0x7FFFu;
                                vgm->header.dac_vol256 = (v > 1024) ? 1024 : v;
                            }
                        }
                    }
                }
            }
        }
        fseek(vgm->fp, cur, SEEK_SET);
    }
    vgm->header.nes_apu_clock = 0;
    vgm->header.has_fds = 0;
    if (vgm->header.version >= 0x161) {
        u8 extra[4];
        long cur = ftell(vgm->fp);
        if (fseek(vgm->fp, 0x84, SEEK_SET) == 0 && fread(extra, 1, 4, vgm->fp) == 4) {
            u32 raw = read_u32le(extra);
            vgm->header.has_fds = (raw & 0x80000000UL) ? 1 : 0;
            vgm->header.nes_apu_clock = raw & 0x3FFFFFFFUL;
        }
        fseek(vgm->fp, cur, SEEK_SET);
    }
    vgm->header.gb_apu_clock = 0;
    if (vgm->header.version >= 0x161) {
        u8 extra[4];
        long cur = ftell(vgm->fp);
        if (fseek(vgm->fp, 0x80, SEEK_SET) == 0 && fread(extra, 1, 4, vgm->fp) == 4)
            vgm->header.gb_apu_clock = read_u32le(extra) & 0x3FFFFFFFUL;
        fseek(vgm->fp, cur, SEEK_SET);
    }
    vgm->header.ym2203_clock = 0;
    if (vgm->header.version >= 0x151) {
        u8 extra[4];
        long cur = ftell(vgm->fp);
        if (fseek(vgm->fp, 0x44, SEEK_SET) == 0 && fread(extra, 1, 4, vgm->fp) == 4)
            vgm->header.ym2203_clock = read_u32le(extra) & 0x3FFFFFFFUL;
        fseek(vgm->fp, cur, SEEK_SET);
    }
    vgm->header.wswan_clock = 0;
    if (vgm->header.version >= 0x171) {
        u8 extra[4];
        long cur = ftell(vgm->fp);
        if (fseek(vgm->fp, 0xC0, SEEK_SET) == 0 && fread(extra, 1, 4, vgm->fp) == 4)
            vgm->header.wswan_clock = read_u32le(extra) & 0x3FFFFFFFUL;
        fseek(vgm->fp, cur, SEEK_SET);
    }
    vgm->header.ym2608_clock = 0;
    if (vgm->header.version >= 0x151) {
        u8 extra[4];
        long cur = ftell(vgm->fp);
        if (fseek(vgm->fp, 0x48, SEEK_SET) == 0 && fread(extra, 1, 4, vgm->fp) == 4)
            vgm->header.ym2608_clock = read_u32le(extra) & 0x3FFFFFFFUL;
        fseek(vgm->fp, cur, SEEK_SET);
    }
    vgm->header.pce_clock = 0;
    if (vgm->header.version >= 0x161) {
        u8 extra[4];
        long cur = ftell(vgm->fp);
        if (fseek(vgm->fp, 0xA4, SEEK_SET) == 0 && fread(extra, 1, 4, vgm->fp) == 4)
            vgm->header.pce_clock = read_u32le(extra) & 0x3FFFFFFFUL;
        fseek(vgm->fp, cur, SEEK_SET);
    }
    vgm->header.rf5c164_clock = 0;
    vgm->inline_left = 0;
    if (vgm->header.version >= 0x151) {
        u8 extra[4];
        long cur = ftell(vgm->fp);
        if (fseek(vgm->fp, 0x6C, SEEK_SET) == 0 && fread(extra, 1, 4, vgm->fp) == 4)
            vgm->header.rf5c164_clock = read_u32le(extra) & 0x3FFFFFFFUL;
        fseek(vgm->fp, cur, SEEK_SET);
    }
    vgm->header.k051649_clock = 0;
    if (vgm->header.version >= 0x151) {
        u8 extra[4];
        long cur = ftell(vgm->fp);
        if (fseek(vgm->fp, 0x9C, SEEK_SET) == 0 && fread(extra, 1, 4, vgm->fp) == 4)
            vgm->header.k051649_clock = read_u32le(extra) & 0x3FFFFFFFUL;
        fseek(vgm->fp, cur, SEEK_SET);
    }
    vgm->header.ym3812_clock = 0;
    vgm->header.ym3526_clock = 0;
    vgm->header.y8950_clock = 0;
    vgm->header.ymf262_clock = 0;
    if (vgm->header.version >= 0x151) {
        u8 extra[4];
        long cur = ftell(vgm->fp);
        if (fseek(vgm->fp, 0x50, SEEK_SET) == 0 && fread(extra, 1, 4, vgm->fp) == 4)
            vgm->header.ym3812_clock = read_u32le(extra) & 0x3FFFFFFFUL;
        if (fseek(vgm->fp, 0x54, SEEK_SET) == 0 && fread(extra, 1, 4, vgm->fp) == 4)
            vgm->header.ym3526_clock = read_u32le(extra) & 0x3FFFFFFFUL;
        if (fseek(vgm->fp, 0x58, SEEK_SET) == 0 && fread(extra, 1, 4, vgm->fp) == 4)
            vgm->header.y8950_clock = read_u32le(extra) & 0x3FFFFFFFUL;
        if (fseek(vgm->fp, 0x5C, SEEK_SET) == 0 && fread(extra, 1, 4, vgm->fp) == 4)
            vgm->header.ymf262_clock = read_u32le(extra) & 0x3FFFFFFFUL;
        fseek(vgm->fp, cur, SEEK_SET);
    }
    if (vgm->header.version >= 0x150) {
        u32 rel = read_u32le(&hdr[0x34]);
        if (rel == 0)
            vgm->header.data_offset = 0x40;
        else
            vgm->header.data_offset = 0x34 + rel;
    } else {
        vgm->header.data_offset = 0x40;
    }
    vgm->data_start = (long)vgm->header.data_offset;
    if (vgm->header.loop_offset != 0)
        vgm->loop_start = 0x1C + (long)vgm->header.loop_offset;
    else
        vgm->loop_start = -1;
    vgm->loop_count = 0;
    vgm->max_loops = (vgm->header.loop_samples != 0 &&
                       vgm->header.loop_samples < VGM_LOOP_EXTEND_CAP_SAMPLES)
                      ? VGM_DEFAULT_LOOPS : 0;
    vgm->rb_base = vgm->data_start;
    vgm->rb_pos = vgm->rb_len = 0;
    if (vgm->lz) {
        vgm->lz_comp_start = (long)read_u32le(&hdr[0x04]);
        lz_reset(vgm);
        if (fseek(vgm->fp, vgm->lz_comp_start, SEEK_SET) != 0) {
            fclose(vgm->fp);
            return 4;
        }
        return 0;
    }
    if (fseek(vgm->fp, vgm->data_start, SEEK_SET) != 0) {
        fprintf(stderr, "vgm_open: seek to data_offset failed\n");
        fclose(vgm->fp);
        return 4;
    }
    return 0;
}

void vgm_close(VgmFile *vgm)
{
    if (vgm->fp) {
        fclose(vgm->fp);
        vgm->fp = NULL;
    }
    if (vgm->pcm_fp) {
        fclose(vgm->pcm_fp);
        vgm->pcm_fp = NULL;
    }
}

static int rb_getc_slow(VgmFile *v)
{
    v->rb_base += (long)v->rb_len;
    if (v->lz) {
        unsigned n = sizeof(v->rbuf);
        if (v->lz_out < v->loop_start && v->loop_start - v->lz_out < (long)n)
            n = (unsigned)(v->loop_start - v->lz_out);
        if (v->lz_out == v->loop_start && !v->lz_ck_ok) lz_save(v);
        v->rb_base = v->lz_out;
        v->rb_len = lz_decode(v, v->rbuf, n);
        v->rb_pos = 0;
        if (v->rb_len == 0) return EOF;
        return v->rbuf[v->rb_pos++];
    }
    v->rb_len = (unsigned)fread(v->rbuf, 1, sizeof(v->rbuf), v->fp);
    v->rb_pos = 0;
    if (v->rb_len == 0) return EOF;
    return v->rbuf[v->rb_pos++];
}

#define RB_GETC(v) ((v)->rb_pos < (v)->rb_len ? (int)(v)->rbuf[(v)->rb_pos++] : rb_getc_slow(v))

static unsigned rb_read(VgmFile *v, u8 *dst, unsigned n)
{
    unsigned i = 0;
    while (i < n) {
        unsigned k = v->rb_len - v->rb_pos;
        int c;
        if (k) {
            if (k > n - i) k = n - i;
            memcpy(dst + i, v->rbuf + v->rb_pos, k);
            v->rb_pos += k;
            i += k;
            continue;
        }
        c = rb_getc_slow(v);
        if (c == EOF) break;
        dst[i++] = (u8)c;
    }
    return i;
}

#define RB_TELL(v) ((v)->rb_base + (long)(v)->rb_pos)

static int rb_seek(VgmFile *v, long pos)
{
    if (pos >= v->rb_base && pos < v->rb_base + (long)v->rb_len) {
        v->rb_pos = (unsigned)(pos - v->rb_base);
        return 0;
    }
    if (v->lz) {
        static unsigned char skipbuf[64];
        if (pos == v->loop_start && v->lz_ck_ok) {
            if (lz_restore(v) != 0) return -1;
        } else if (pos < v->lz_out) {
            if (fseek(v->fp, v->lz_comp_start, SEEK_SET) != 0) return -1;
            lz_reset(v);
        }
        while (v->lz_out < pos) {
            unsigned n = (pos - v->lz_out > (long)sizeof(skipbuf)) ? sizeof(skipbuf) : (unsigned)(pos - v->lz_out);
            if (v->lz_out == v->loop_start && !v->lz_ck_ok) lz_save(v);
            if (lz_decode(v, skipbuf, n) == 0) return -1;
        }
        v->rb_base = v->lz_out;
        v->rb_len = 0;
        v->rb_pos = 0;
        return 0;
    }
    if (fseek(v->fp, pos, SEEK_SET) != 0) return -1;
    v->rb_base = pos;
    v->rb_len = 0;
    v->rb_pos = 0;
    return 0;
}

int vgm_next_event(VgmFile *vgm, VgmEvent *ev)
{
    int cmd;
    u8 buf[4];
    unsigned int cmdgroup, subaddr, data, real_addr;
    u32 total_wait;
    int c2;
    u8 pcmbyte;
    u32 remaining_bank, want;
    u8 mbuf[6];
    if (vgm->inline_left) {
        if (rb_seek(vgm, RB_TELL(vgm) + (long)vgm->inline_left) != 0) goto trunc0;
        vgm->inline_left = 0;
    }
    cmd = RB_GETC(vgm);
    if (cmd == EOF) {
        ev->type = VGM_EV_END;
        return 1;
    }
    switch (cmd) {
        case VGM_CMD_NES_APU_WRITE:
            if (rb_read(vgm, buf, (unsigned)(2)) != 2) goto trunc;
            ev->type = VGM_EV_NES_APU_WRITE;
            ev->addr = buf[0];
            ev->data = buf[1];
            return 0;
        case 0xD2:
            if (rb_read(vgm, buf, (unsigned)(3)) != 3) goto trunc;
            cmdgroup = buf[0] & 0x7F;
            subaddr = buf[1];
            data = buf[2];
            switch (cmdgroup) {
                case 0x00: real_addr = subaddr; break;
                case 0x01: real_addr = 0x80 + subaddr; break;
                case 0x02: real_addr = 0x8A + subaddr; break;
                case 0x03: real_addr = 0x8F; break;
                case 0x04: real_addr = 0x100 + subaddr; break;
                default: real_addr = 0xFF; break;
            }
            ev->type = VGM_EV_SCC_WRITE;
            ev->addr = real_addr;
            ev->data = (u8)data;
            return 0;
        case VGM_CMD_WAIT_N16:
            if (rb_read(vgm, buf, (unsigned)(2)) != 2) goto trunc;
            ev->type = VGM_EV_WAIT;
            ev->wait = (u32)buf[0] | ((u32)buf[1] << 8);
            return 0;
        case VGM_CMD_WAIT_735:
            ev->type = VGM_EV_WAIT;
            ev->wait = 735;
            return 0;
        case VGM_CMD_WAIT_882:
            ev->type = VGM_EV_WAIT;
            ev->wait = 882;
            return 0;
        case VGM_CMD_PSG_WRITE:
            if (rb_read(vgm, buf, (unsigned)(1)) != 1) goto trunc;
            ev->type = VGM_EV_PSG_WRITE;
            ev->data = buf[0];
            return 0;
        case VGM_CMD_PSG2_WRITE:
            if (rb_read(vgm, buf, (unsigned)(1)) != 1) goto trunc;
            ev->type = VGM_EV_PSG2_WRITE;
            ev->data = buf[0];
            return 0;
        case VGM_CMD_AY8910_WRITE:
            if (rb_read(vgm, buf, (unsigned)(2)) != 2) goto trunc;
            ev->type = VGM_EV_AY8910_WRITE;
            ev->addr = buf[0];
            ev->data = buf[1];
            return 0;
        case VGM_CMD_GB_APU_WRITE:
            if (rb_read(vgm, buf, (unsigned)(2)) != 2) goto trunc;
            ev->type = VGM_EV_GB_APU_WRITE;
            ev->addr = buf[0];
            ev->data = buf[1];
            return 0;
        case VGM_CMD_PCE_WRITE:
            if (rb_read(vgm, buf, (unsigned)(2)) != 2) goto trunc;
            ev->type = VGM_EV_PCE_WRITE;
            ev->addr = buf[0];
            ev->data = buf[1];
            return 0;
        case VGM_CMD_GG_STEREO:
            if (rb_read(vgm, buf, (unsigned)(1)) != 1) goto trunc;
            ev->type = VGM_EV_UNKNOWN;
            return 0;
        case VGM_CMD_YM2612_PORT0:
        case VGM_CMD_YM2612_PORT1:
            if (rb_read(vgm, buf, (unsigned)(2)) != 2) goto trunc;
            ev->type = VGM_EV_YM2612_WRITE;
            ev->addr = (cmd == VGM_CMD_YM2612_PORT1) ? (0x100U | buf[0]) : buf[0];
            ev->data = buf[1];
            return 0;
        case VGM_CMD_YM2413_WRITE:
            if (rb_read(vgm, buf, (unsigned)(2)) != 2) goto trunc;
            ev->type = VGM_EV_YM2413_WRITE;
            ev->addr = buf[0];
            ev->data = buf[1];
            return 0;
        case VGM_CMD_YM2203_WRITE:
            if (rb_read(vgm, buf, (unsigned)(2)) != 2) goto trunc;
            ev->type = VGM_EV_YM2203_WRITE;
            ev->addr = buf[0];
            ev->data = buf[1];
            return 0;
        case VGM_CMD_YM2608_PORT0:
        case VGM_CMD_YM2608_PORT1:
            if (rb_read(vgm, buf, (unsigned)(2)) != 2) goto trunc;
            ev->type = VGM_EV_YM2608_WRITE;
            ev->addr = (cmd == VGM_CMD_YM2608_PORT1) ? (0x100U | buf[0]) : buf[0];
            ev->data = buf[1];
            return 0;
        case VGM_CMD_YM3812_WRITE:
            if (rb_read(vgm, buf, (unsigned)(2)) != 2) goto trunc;
            ev->type = VGM_EV_YM3812_WRITE;
            ev->addr = buf[0];
            ev->data = buf[1];
            return 0;
        case VGM_CMD_YM3526_WRITE:
            if (rb_read(vgm, buf, (unsigned)(2)) != 2) goto trunc;
            ev->type = VGM_EV_YM3526_WRITE;
            ev->addr = buf[0];
            ev->data = buf[1];
            return 0;
        case VGM_CMD_Y8950_WRITE:
            if (rb_read(vgm, buf, (unsigned)(2)) != 2) goto trunc;
            ev->type = VGM_EV_Y8950_WRITE;
            ev->addr = buf[0];
            ev->data = buf[1];
            return 0;
        case VGM_CMD_YMF262_PORT0:
        case VGM_CMD_YMF262_PORT1:
            if (rb_read(vgm, buf, (unsigned)(2)) != 2) goto trunc;
            ev->type = VGM_EV_YMF262_WRITE;
            ev->addr = (cmd == VGM_CMD_YMF262_PORT1) ? (0x100U | buf[0]) : buf[0];
            ev->data = buf[1];
            return 0;
        case VGM_CMD_WSWAN_PORT:
            if (rb_read(vgm, buf, (unsigned)(2)) != 2) goto trunc;
            ev->type = VGM_EV_WSWAN_PORT;
            ev->addr = 0x80 | (buf[0] & 0x7F);
            ev->data = buf[1];
            return 0;
        case VGM_CMD_WSWAN_MEM:
            if (rb_read(vgm, mbuf, (unsigned)(3)) != 3) goto trunc;
            ev->type = VGM_EV_WSWAN_MEM;
            ev->addr = (((u32)mbuf[0] & 0x7F) << 8) | (u32)mbuf[1];
            ev->data = mbuf[2];
            return 0;
        case VGM_CMD_PCM_SEEK:
            if (rb_read(vgm, buf, (unsigned)(4)) != 4) goto trunc;
            vgm->pcm_read_pos = read_u32le(buf);
            ev->type = VGM_EV_UNKNOWN;
            return 0;
        case 0xB1:
            if (rb_read(vgm, buf, (unsigned)(2)) != 2) goto trunc;
            ev->type = VGM_EV_RF5C_WRITE;
            ev->addr = buf[0];
            ev->data = buf[1];
            return 0;
        case 0xC2:
            if (rb_read(vgm, buf, (unsigned)(3)) != 3) goto trunc;
            ev->type = VGM_EV_RF5C_MEM;
            ev->addr = (u32)buf[0] | ((u32)buf[1] << 8);
            ev->data = buf[2];
            return 0;
        case 0x68:
            {
                u8 sk[11];
                if (rb_read(vgm, sk, (unsigned)(11)) != 11) goto trunc;
                ev->type = VGM_EV_UNKNOWN;
                if (sk[1] == 0x02) {
                    ev->type = VGM_EV_RF5C_COPY;
                    ev->addr = (u32)sk[2] | ((u32)sk[3] << 8) | ((u32)sk[4] << 16);
                    vgm->rf_dst = (unsigned)sk[5] | ((unsigned)sk[6] << 8);
                    ev->wait = (u32)sk[8] | ((u32)sk[9] << 8) | ((u32)sk[10] << 16);
                    if (ev->wait == 0) ev->wait = 0x1000000UL;
                }
                return 0;
            }
        case VGM_CMD_DATA_BLOCK:
            if (rb_read(vgm, mbuf, (unsigned)(6)) != 6) goto trunc;
            want = read_u32le(&mbuf[2]) & 0x7FFFFFFFUL;
            ev->type = VGM_EV_UNKNOWN;
            if (mbuf[1] == 0xC0 || mbuf[1] == 0xC1) {
                vgm->inline_left = want;
                if (mbuf[1] == 0xC0 && want >= 4) {
                    if (rb_read(vgm, buf, 2) != 2) goto trunc;
                    vgm->inline_left = want - 2;
                    if (buf[0] == 0xFE && buf[1] == 0xFF) {
                        ev->type = VGM_EV_RF5C_TABLE;
                        ev->wait = want - 2;
                    }
                    return 0;
                }
                if (mbuf[1] == 0xC1 && want >= 2) {
                    if (rb_read(vgm, buf, 2) != 2) goto trunc;
                    vgm->inline_left = want - 2;
                    ev->type = VGM_EV_RF5C_RAM;
                    ev->addr = (u32)buf[0] | ((u32)buf[1] << 8);
                    ev->wait = want - 2;
                }
                return 0;
            }
            if (vgm->lz) {
                long ext;
                if (rb_read(vgm, buf, 4) != 4) goto trunc;
                ext = (long)read_u32le(buf);
                if (mbuf[1] == 0x00) {
                    vgm->pcm_bank_offset = ext;
                    vgm->pcm_bank_size = want;
                    vgm->pcm_read_pos = 0;
                    ev->type = VGM_EV_PCM_BANK;
                    ev->wait = want;
                } else if (mbuf[1] == 0x3F) {
                    vgm->vdac_offset = ext;
                    ev->type = VGM_EV_DAC_TABLE;
                    ev->wait = want;
                } else if (mbuf[1] == 0xC2) {
                    vgm->pcm_bank_offset = ext;
                    ev->type = VGM_EV_NES_RAM;
                    ev->wait = want;
                } else if (mbuf[1] == 0xDF) {
                    vgm->pcm_bank_offset = ext;
                    ev->type = VGM_EV_NES_PCM;
                    ev->wait = want;
                } else if (mbuf[1] == 0x02) {
                    vgm->pcm_bank_offset = ext;
                    ev->type = VGM_EV_RF5C_BANK;
                    ev->wait = want;
                }
                return 0;
            }
            if (mbuf[1] == 0x00) {
                vgm->pcm_bank_offset = RB_TELL(vgm);
                vgm->pcm_bank_size = want;
                vgm->pcm_read_pos = 0;
                ev->type = VGM_EV_PCM_BANK;
                ev->wait = want;
            } else if (mbuf[1] == 0x3F) {
                vgm->vdac_offset = RB_TELL(vgm);
                ev->type = VGM_EV_DAC_TABLE;
                ev->wait = want;
            } else if (mbuf[1] == 0xC2) {
                vgm->pcm_bank_offset = RB_TELL(vgm);
                ev->type = VGM_EV_NES_RAM;
                ev->wait = want;
            } else if (mbuf[1] == 0xDF) {
                vgm->pcm_bank_offset = RB_TELL(vgm);
                ev->type = VGM_EV_NES_PCM;
                ev->wait = want;
            } else if (mbuf[1] == 0x02) {
                vgm->pcm_bank_offset = RB_TELL(vgm);
                ev->type = VGM_EV_RF5C_BANK;
                ev->wait = want;
            }
            if (rb_seek(vgm, RB_TELL(vgm) + (long)want) != 0) goto trunc;
            return 0;
        case VGM_CMD_END:
            if (vgm->loop_start >= 0 && vgm->loop_count < vgm->max_loops) {
                vgm->loop_count++;
                if (rb_seek(vgm, vgm->loop_start) != 0) {
                    ev->type = VGM_EV_END;
                    return 1;
                }
                return vgm_next_event(vgm, ev);
            }
            ev->type = VGM_EV_END;
            return 1;
        default:
            if (cmd >= VGM_CMD_WAIT_SMALL_LO && cmd <= VGM_CMD_WAIT_SMALL_HI) {
                ev->type = VGM_EV_WAIT;
                ev->wait = (u32)(cmd & 0x0F) + 1;
                return 0;
            }
            if (cmd >= VGM_CMD_YM2612_PCM_LO && cmd <= VGM_CMD_YM2612_PCM_HI) {
                total_wait = (u32)(cmd & 0x0F);
                for (;;) {
                    c2 = RB_GETC(vgm);
                    if (c2 >= VGM_CMD_YM2612_PCM_LO && c2 <= VGM_CMD_YM2612_PCM_HI)
                        total_wait += (u32)(c2 & 0x0F);
                    else if (c2 >= VGM_CMD_WAIT_SMALL_LO && c2 <= VGM_CMD_WAIT_SMALL_HI)
                        total_wait += (u32)(c2 & 0x0F) + 1;
                    else {
                        if (c2 != EOF) vgm->rb_pos--;
                        break;
                    }
                }
                if (total_wait == 0) {
                    ev->type = VGM_EV_UNKNOWN;
                    return 0;
                }
                ev->type = VGM_EV_WAIT;
                ev->wait = total_wait;
                return 0;
            }
            if (cmd == 0x92) {
                if (rb_read(vgm, mbuf, (unsigned)(5)) != 5) goto trunc;
                ev->type = VGM_EV_DAC_FREQ;
                ev->wait = read_u32le(&mbuf[1]);
                ev->data = mbuf[0];
                return 0;
            }
            if (cmd == 0x93) {
                u8 sb[10];
                if (rb_read(vgm, sb, (unsigned)(10)) != 10) goto trunc;
                ev->type = VGM_EV_DAC_START;
                ev->addr = read_u32le(&sb[1]);
                ev->data = (u8)((sb[0] << 4) | (sb[5] & 0x0F));
                ev->wait = read_u32le(&sb[6]);
                return 0;
            }
            if (cmd == 0x94) {
                if (rb_read(vgm, mbuf, (unsigned)(1)) != 1) goto trunc;
                ev->type = VGM_EV_DAC_STOP;
                ev->data = mbuf[0];
                return 0;
            }
            {
                int skip = -1;
                u8 sk[12];
                if (cmd >= 0x30 && cmd <= 0x3F) skip = 1;
                else if (cmd >= 0x40 && cmd <= 0x4E) skip = 2;
                else if (cmd >= 0x51 && cmd <= 0x5F) skip = 2;
                else if (cmd == 0x68) skip = 11;
                else if (cmd == 0x90 || cmd == 0x91 || cmd == 0x95) skip = 4;
                else if (cmd == 0x92) skip = 5;
                else if (cmd == 0x93) skip = 10;
                else if (cmd == 0x94) skip = 1;
                else if (cmd >= 0xA0 && cmd <= 0xBF) skip = 2;
                else if (cmd >= 0xC0 && cmd <= 0xDF) skip = 3;
                else if (cmd >= 0xE1 && cmd <= 0xFF) skip = 4;
                if (skip >= 0) {
                    if (skip > 0 && rb_read(vgm, sk, (unsigned)((size_t)skip)) != (size_t)skip) goto trunc;
                    ev->type = VGM_EV_UNKNOWN;
                    return 0;
                }
            }
            fprintf(stderr, "vgm_next_event: unknown command 0x%02X at offset %lu\n",
                    cmd, RB_TELL(vgm) - 1);
            ev->type = VGM_EV_END;
            return 1;
    }
trunc:
    fprintf(stderr, "vgm_next_event: truncated file reading command 0x%02X\n", cmd);
    ev->type = VGM_EV_END;
    return 1;
trunc0:
    ev->type = VGM_EV_END;
    return 1;
}

unsigned vgm_read_inline(VgmFile *vgm, unsigned char *dst, unsigned n)
{
    unsigned got;
    if ((u32)n > vgm->inline_left) n = (unsigned)vgm->inline_left;
    got = rb_read(vgm, dst, n);
    vgm->inline_left -= got;
    if (got < n) vgm->inline_left = 0;
    return got;
}

void vgm_prewarm(VgmFile *vgm)
{
    static unsigned char scratch[4096];
    long saved_pos = ftell(vgm->fp);
    fseek(vgm->fp, 0, SEEK_SET);
    while (fread(scratch, 1, sizeof(scratch), vgm->fp) == sizeof(scratch))
        ;
    fseek(vgm->fp, saved_pos, SEEK_SET);
}

static void vgm_read_utf16_ascii(FILE *fp, char *out, int maxlen)
{
    int i = 0;
    unsigned int word;
    for (;;) {
        if (fread(&word, 2, 1, fp) != 1) break;
        if (word == 0) break;
        if (i < maxlen - 1) {
            out[i] = ((word >> 8) == 0 && (word & 0x00FF) < 128) ? (char)(word & 0x00FF) : '?';
            i++;
        }
    }
    out[i < maxlen ? i : maxlen - 1] = '\0';
}

static void vgm_skip_utf16_string(FILE *fp)
{
    unsigned int word;
    for (;;) {
        if (fread(&word, 2, 1, fp) != 1) break;
        if (word == 0) break;
    }
}

int vgm_read_gd3(VgmFile *vgm, VgmTag *tag)
{
    long abs_gd3, saved_pos;
    u8 magic[4];
    unsigned int len_words;
    unsigned int skip_word;
    memset(tag, 0, sizeof(*tag));
    if (vgm->header.gd3_offset == 0)
        return 0;
    abs_gd3 = 0x14 + (long)vgm->header.gd3_offset;
    saved_pos = ftell(vgm->fp);
    if (fseek(vgm->fp, abs_gd3, SEEK_SET) != 0) { fseek(vgm->fp, saved_pos, SEEK_SET); return 0; }
    if (fread(magic, 1, 4, vgm->fp) != 4) { fseek(vgm->fp, saved_pos, SEEK_SET); return 0; }
    if (magic[0]!='G' || magic[1]!='d' || magic[2]!='3' || magic[3]!=' ') {
        fseek(vgm->fp, saved_pos, SEEK_SET);
        return 0;
    }
    fseek(vgm->fp, 4, SEEK_CUR);
    if (fread(&len_words, 2, 1, vgm->fp) != 1) { fseek(vgm->fp, saved_pos, SEEK_SET); return 0; }
    fseek(vgm->fp, 2, SEEK_CUR);
    vgm_read_utf16_ascii(vgm->fp, tag->track_name, VGM_TAG_MAXLEN);
    for (;;) {
        if (fread(&skip_word, 2, 1, vgm->fp) != 1) break;
        if (skip_word == 0) break;
    }
    vgm_read_utf16_ascii(vgm->fp, tag->game_name, VGM_TAG_MAXLEN);
    for (;;) {
        if (fread(&skip_word, 2, 1, vgm->fp) != 1) break;
        if (skip_word == 0) break;
    }
    vgm_read_utf16_ascii(vgm->fp, tag->system_name, VGM_TAG_MAXLEN);
    for (;;) {
        if (fread(&skip_word, 2, 1, vgm->fp) != 1) break;
        if (skip_word == 0) break;
    }
    vgm_read_utf16_ascii(vgm->fp, tag->author_name, VGM_TAG_MAXLEN);
    for (;;) {
        if (fread(&skip_word, 2, 1, vgm->fp) != 1) break;
        if (skip_word == 0) break;
    }
    vgm_read_utf16_ascii(vgm->fp, tag->release_date, VGM_TAG_MAXLEN);
    vgm_read_utf16_ascii(vgm->fp, tag->vgm_by, VGM_TAG_MAXLEN);
    vgm_read_utf16_ascii(vgm->fp, tag->notes, VGM_NOTES_MAXLEN);
    tag->has_gd3 = 1;
    fseek(vgm->fp, saved_pos, SEEK_SET);
    return 1;
}
