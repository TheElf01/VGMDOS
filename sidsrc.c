#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "vgm.h"
#include "sidsrc.h"
#include "dosmem.h"
#include "c6502a.h"

#define PSID_HDR      0x7C
#define SONG_SECS     180UL
#define INIT_MAX      1000000UL
#define PLAY_MAX      30000UL
#define WQ_MAX        512
#define PAL_CLK       985248UL
#define NTSC_CLK      1022727UL

static unsigned ramseg;
static unsigned char far *ram;
static DosBuffer wqb;
static unsigned far *wq;
static unsigned wq_n, wq_h;
static unsigned char ciat[2];
static C6 cp;

static void io_write(unsigned a, unsigned char v)
{
    if ((a >> 12) == 0xD && (ram[1] & 3) && (ram[1] & 4)) {
        if (a >= 0xD400u && a < 0xD800u) {
            if (wq_n < WQ_MAX) wq[wq_n++] = ((a & 31) << 8) | v;
        } else if (a == 0xDC04u) ciat[0] = v;
        else if (a == 0xDC05u) ciat[1] = v;
    }
}

static int started, done, waitpend, cia_speed, song, sid_ok;
static unsigned load_addr, init_addr, play_addr;
static unsigned long data_off, data_len, period, nframes, frames, wait_acc;

static unsigned be16(const unsigned char *p)
{
    return (unsigned)((p[0] << 8) | p[1]);
}

static unsigned long be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | p[3];
}

static void call6502(unsigned addr, unsigned char a, unsigned long maxticks)
{
    unsigned long left = maxticks;
    unsigned chunk;

    ram[0x1FF] = 0x02;
    ram[0x1FE] = 0xFF;
    cp.s = 0xFD;
    cp.a = a;
    cp.pc = addr;
    cp.ramseg = ramseg;
    cp.ticks = 0;
    for (;;) {
        if (cp.ticks == 0) {
            chunk = left > 60000UL ? 60000U : (unsigned)left;
            if (chunk == 0) break;
            cp.ticks = chunk;
            left -= chunk;
        }
        c6_run(&cp);
        if (cp.status == 2) {
            io_write(cp.io_a, cp.io_v);
            continue;
        }
        if (cp.status == 1) break;
    }
}

static void cpu_reset(void)
{
    _fmemset(ram, 0, 0x8000U);
    _fmemset(ram + 0x8000U, 0, 0x8000U);
    ram[1] = 0x37;
    ram[0x300] = 0x02;
    ciat[0] = ciat[1] = 0;
    memset(&cp, 0, sizeof(cp));
    cp.ramseg = ramseg;
}

static int load_image(VgmFile *v)
{
    static unsigned char tmp[256];
    unsigned long done_b = 0;
    unsigned n, want;

    if (fseek(v->fp, (long)data_off, SEEK_SET) != 0) return 1;
    while (done_b < data_len) {
        want = data_len - done_b > sizeof(tmp) ? (unsigned)sizeof(tmp) : (unsigned)(data_len - done_b);
        n = (unsigned)fread(tmp, 1, want, v->fp);
        if (n == 0) break;
        _fmemcpy(ram + (load_addr + (unsigned)done_b), tmp, n);
        done_b += n;
    }
    return 0;
}

static int start_tune(VgmFile *v)
{
    unsigned lat;

    if (_dos_allocmem(4096, &ramseg) != 0) { ramseg = 0; return 1; }
    ram = (unsigned char far *)MK_FP(ramseg, 0);
    if (!dosmem_alloc_simple(&wqb, WQ_MAX * 2)) return 1;
    wq = (unsigned far *)wqb.ptr;
    cpu_reset();
    if (load_image(v)) return 1;
    wq_n = wq_h = 0;
    call6502(init_addr, (unsigned char)(song - 1), INIT_MAX);
    if (cp.status != 1 || cp.pc != 0x300u) return 1;
    lat = ciat[0] | ((unsigned)ciat[1] << 8);
    if (cia_speed && lat > 100) period = (unsigned long)lat + lat / 66U;
    else if (cia_speed) period = 16667UL;
    else period = (v->header.sid_clock == NTSC_CLK) ? 16683UL : 19950UL;
    nframes = SONG_SECS * 1000000UL / period;
    return 0;
}

int sidsrc_open(VgmFile *v)
{
    unsigned char hdr[PSID_HDR];
    unsigned n, flags;
    unsigned long speed, end, flen;
    int nsongs;

    if (fseek(v->fp, 0L, SEEK_SET) != 0) return 1;
    n = (unsigned)fread(hdr, 1, PSID_HDR, v->fp);
    if (n < 0x76 || memcmp(hdr, "PSID", 4) != 0) return 2;
    for (; n < PSID_HDR; n++) hdr[n] = 0;
    data_off = be16(hdr + 6);
    load_addr = be16(hdr + 8);
    init_addr = be16(hdr + 10);
    play_addr = be16(hdr + 12);
    nsongs = (int)be16(hdr + 14);
    song = (int)be16(hdr + 16);
    speed = be32(hdr + 18);
    flags = be16(hdr + 0x76);
    if (nsongs < 1) nsongs = 1;
    if (song < 1 || song > nsongs) song = 1;
    cia_speed = (song <= 32) ? (int)((speed >> (song - 1)) & 1UL) : 0;
    if ((flags & 1) || play_addr == 0 || data_off < PSID_HDR) return 3;
    fseek(v->fp, 0L, SEEK_END);
    flen = (unsigned long)ftell(v->fp);
    if (flen < data_off + 2UL) return 3;
    data_len = flen - data_off;
    if (load_addr == 0) {
        unsigned char b[2];
        fseek(v->fp, (long)data_off, SEEK_SET);
        if (fread(b, 1, 2, v->fp) != 2) return 3;
        load_addr = b[0] | ((unsigned)b[1] << 8);
        data_off += 2;
        data_len -= 2;
    }
    end = (unsigned long)load_addr + data_len;
    if (load_addr < 0x0400u || end > 0xD000UL || data_len == 0) return 3;
    v->header.sid_clock = (((flags >> 2) & 3) == 2) ? NTSC_CLK : PAL_CLK;
    v->header.version = (unsigned long)be16(hdr + 4) << 8;
    v->header.total_samples = SONG_SECS * 44100UL;
    v->sid = 1;
    started = done = waitpend = 0;
    frames = wait_acc = 0;
    sid_ok = 1;
    return 0;
}

int sidsrc_next(VgmFile *v, VgmEvent *ev)
{
    unsigned long w;

    for (;;) {
        if (wq_h < wq_n) {
            ev->type = VGM_EV_SID_WRITE;
            ev->addr = wq[wq_h] >> 8;
            ev->data = (u8)(wq[wq_h] & 255);
            ev->wait = 0;
            wq_h++;
            return 0;
        }
        if (waitpend) {
            waitpend = 0;
            wait_acc += period * 44100UL;
            w = wait_acc / 1000000UL;
            wait_acc %= 1000000UL;
            ev->type = VGM_EV_WAIT;
            ev->addr = 0;
            ev->data = 0;
            ev->wait = w;
            return 0;
        }
        if (done || !sid_ok) break;
        wq_n = wq_h = 0;
        if (!started) {
            started = 1;
            if (start_tune(v)) { done = 1; break; }
        } else {
            if (frames >= nframes) { done = 1; break; }
            call6502(play_addr, 0, PLAY_MAX);
            if (cp.status != 1 || cp.pc != 0x300u) { done = 1; break; }
            frames++;
        }
        waitpend = 1;
    }
    ev->type = VGM_EV_END;
    ev->addr = 0;
    ev->data = 0;
    ev->wait = 0;
    return 1;
}

void sidsrc_close(void)
{
    if (ramseg) _dos_freemem(ramseg);
    ramseg = 0;
    ram = 0;
    if (wq) dosmem_free(&wqb);
    wq = 0;
    started = done = waitpend = 0;
    sid_ok = 0;
    wq_n = wq_h = 0;
}

static unsigned char to437(unsigned char c)
{
    static const unsigned char from[] = { 0xFC, 0xF6, 0xE4, 0xE9, 0xDF, 0xC4, 0xD6, 0xDC, 0xF1, 0xE8, 0xE0, 0xE7, 0xEA, 0xE2, 0xEE, 0xF4, 0xFB, 0xE1, 0xED, 0xF3, 0xFA, 0xD1, 0xE5, 0xC5, 0xE6, 0xC6 };
    static const unsigned char to[] = { 0x81, 0x94, 0x84, 0x82, 0xE1, 0x8E, 0x99, 0x9A, 0xA4, 0x8A, 0x85, 0x87, 0x88, 0x83, 0x8C, 0x93, 0x96, 0xA0, 0xA1, 0xA2, 0xA3, 0xA5, 0x86, 0x8F, 0x91, 0x92 };
    unsigned i;

    if (c < 0x80) return c;
    for (i = 0; i < sizeof(from); i++)
        if (from[i] == c) return to[i];
    return '?';
}

static void tagcpy(char *dst, const unsigned char *src)
{
    int i;

    for (i = 0; i < 32 && i < VGM_TAG_MAXLEN - 1 && src[i]; i++) dst[i] = (char)to437(src[i]);
    dst[i] = 0;
}

void sidsrc_tag(VgmFile *v, VgmTag *tag)
{
    unsigned char hdr[PSID_HDR];
    long pos = ftell(v->fp);
    unsigned n;

    memset(tag, 0, sizeof(*tag));
    if (fseek(v->fp, 0L, SEEK_SET) == 0) {
        n = (unsigned)fread(hdr, 1, PSID_HDR, v->fp);
        if (n >= 0x76) {
            tag->has_gd3 = 1;
            tagcpy(tag->track_name, hdr + 0x16);
            tagcpy(tag->author_name, hdr + 0x36);
            tagcpy(tag->release_date, hdr + 0x56);
            strcpy(tag->system_name, "Commodore 64");
        }
    }
    fseek(v->fp, pos, SEEK_SET);
}
