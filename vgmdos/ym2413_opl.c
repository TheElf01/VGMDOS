#include <string.h>
#include "ym2413_opl.h"
#include "opl2.h"

#ifdef __WATCOMC__
#define YM2413OPL_FAR __far
#else
#define YM2413OPL_FAR
#endif

typedef unsigned char u8b;
typedef unsigned short u16b;

typedef struct {
    u8b MKS, CKS;
    u8b MML, CML;
    u8b MA,  CA;
    u8b MSL, CSL;
    u8b MS,  CS;
    u8b MD,  CD;
    u8b MR,  CR;
    u8b MTL, CTL;
    u8b MEV, CEV;
    u8b MW,  CW;
    u8b FB,  CON;
    u8b MAM, CAM;
    u8b MVIB, CVIB;
} OplPatch;

static const u8b ym2413_rom[19][8] = {
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    { 0x71,0x61,0x1e,0x17,0xd0,0x78,0x00,0x17 },
    { 0x13,0x41,0x1a,0x0d,0xd8,0xf7,0x23,0x13 },
    { 0x13,0x01,0x99,0x00,0xf2,0xc4,0x21,0x23 },
    { 0x11,0x61,0x0e,0x07,0x8d,0x64,0x70,0x27 },
    { 0x32,0x21,0x1e,0x06,0xe1,0x76,0x01,0x28 },
    { 0x31,0x22,0x16,0x05,0xe0,0x71,0x00,0x18 },
    { 0x21,0x61,0x1d,0x07,0x82,0x81,0x11,0x07 },
    { 0x33,0x21,0x2d,0x13,0xb0,0x70,0x00,0x07 },
    { 0x61,0x61,0x1b,0x06,0x64,0x65,0x10,0x17 },
    { 0x41,0x61,0x0b,0x18,0x85,0xf0,0x81,0x07 },
    { 0x33,0x01,0x83,0x11,0xea,0xef,0x10,0x04 },
    { 0x17,0xc1,0x24,0x07,0xf8,0xf8,0x22,0x12 },
    { 0x61,0x50,0x0c,0x05,0xd2,0xf5,0x40,0x42 },
    { 0x01,0x01,0x55,0x03,0xe9,0x90,0x03,0x02 },
    { 0x41,0x41,0x89,0x03,0xf1,0xe4,0xc0,0x13 },
    { 0x01,0x01,0x18,0x0f,0xdf,0xf8,0x6a,0x6d },
    { 0x01,0x01,0x00,0x00,0xc8,0xd8,0xa7,0x68 },
    { 0x05,0x01,0x00,0x00,0xf8,0xaa,0x59,0x55 },
};

static OplPatch patches[19];

static const u8b ksl_map[4] = { 0, 2, 1, 3 };

static void decode_patch(OplPatch *p, const u8b *r)
{
    p->MAM = (r[0] >> 7) & 1; p->MVIB = (r[0] >> 6) & 1; p->MS = (r[0] >> 5) & 1;
    p->MEV = (r[0] >> 4) & 1; p->MML = r[0] & 0x0F;
    p->CAM = (r[1] >> 7) & 1; p->CVIB = (r[1] >> 6) & 1; p->CS = (r[1] >> 5) & 1;
    p->CEV = (r[1] >> 4) & 1; p->CML = r[1] & 0x0F;
    p->MKS = (r[2] >> 6) & 3; p->MTL = (u8b)(0x3F - (r[2] & 0x3F));
    p->CKS = (r[3] >> 6) & 3;
    p->CW = (r[3] >> 4) & 1;
    p->MW = (r[3] >> 3) & 1;
    p->FB = r[3] & 7; p->CON = 0;
    p->MA = (r[4] >> 4) & 0x0F; p->MD = r[4] & 0x0F;
    p->CA = (r[5] >> 4) & 0x0F; p->CD = r[5] & 0x0F;
    p->MSL = (r[6] >> 4) & 0x0F; p->MR = r[6] & 0x0F;
    p->CSL = (r[7] >> 4) & 0x0F; p->CR = r[7] & 0x0F;
    p->CTL = 0;
}

static const u8b fm_vol[16] = {
    0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c,
    0x20, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c
};

static u8b regs[64];
static int  vcref[9];
static int  vlref[9];
static u16b fref[9];
static u16b custom_gen = 0;
static u16b vc_custom_gen[9];
#define RHYTHM_ON (regs[0x0E] & 0x20)

static void set_voice(int ch, int instrument, int vol6)
{
    int r = (ch % 3) + ((ch / 3) * 0x08);
    OplPatch *p;
    vlref[ch] = vol6 & 0x3F;
    if (instrument != 0xFF) {
        if (vcref[ch] != instrument || (instrument == 0 && vc_custom_gen[ch] != custom_gen)) {
            vcref[ch] = instrument;
            vc_custom_gen[ch] = custom_gen;
            p = &patches[instrument];
            opl2_write((u8b)(0x20 + r), (u8b)((p->MAM << 7) | (p->MVIB << 6) | (p->MS << 5) | (p->MEV << 4) | p->MML));
            opl2_write((u8b)(0x60 + r), (u8b)((p->MA << 4) | p->MD));
            opl2_write((u8b)(0x80 + r), (u8b)((p->MSL << 4) | p->MR));
            opl2_write((u8b)(0xE0 + r), p->MW);
            opl2_write((u8b)(0x23 + r), (u8b)((p->CAM << 7) | (p->CVIB << 6) | (p->CS << 5) | (p->CEV << 4) | p->CML));
            opl2_write((u8b)(0x63 + r), (u8b)((p->CA << 4) | p->CD));
            opl2_write((u8b)(0x83 + r), (u8b)((p->CSL << 4) | p->CR));
            opl2_write((u8b)(0xE3 + r), p->CW);
            opl2_write((u8b)(0xC0 + ch), (u8b)((p->FB << 1) | p->CON | 0x30));
            opl2_write((u8b)(0x40 + r), (u8b)((0x3F - p->MTL) | (ksl_map[p->MKS] << 6)));
        }
    }
    p = &patches[vcref[ch]];
    opl2_write((u8b)(0x43 + r), (u8b)((ksl_map[p->CKS] << 6) | vlref[ch]));
}

static void set_user_voice(void)
{
    int ch, lim = RHYTHM_ON ? 6 : 9;
    custom_gen++;
    for (ch = 0; ch < lim; ch++)
        if (!vcref[ch])
            set_voice(ch, 0, vlref[ch]);
}

static void rhythm_op(int off, const OplPatch *p, int car)
{
    if (!car) {
        opl2_write((u8b)(0x20 + off), (u8b)((p->MAM << 7) | (p->MVIB << 6) | (p->MS << 5) | (p->MEV << 4) | p->MML));
        opl2_write((u8b)(0x60 + off), (u8b)((p->MA << 4) | p->MD));
        opl2_write((u8b)(0x80 + off), (u8b)((p->MSL << 4) | p->MR));
        opl2_write((u8b)(0xE0 + off), p->MW);
        opl2_write((u8b)(0x40 + off), (u8b)((0x3F - p->MTL) | (ksl_map[p->MKS] << 6)));
    } else {
        opl2_write((u8b)(0x20 + off), (u8b)((p->CAM << 7) | (p->CVIB << 6) | (p->CS << 5) | (p->CEV << 4) | p->CML));
        opl2_write((u8b)(0x60 + off), (u8b)((p->CA << 4) | p->CD));
        opl2_write((u8b)(0x80 + off), (u8b)((p->CSL << 4) | p->CR));
        opl2_write((u8b)(0xE0 + off), p->CW);
    }
}

static void rhythm_vols(void)
{
    opl2_write(0x53, (u8b)((ksl_map[patches[16].CKS] << 6) | fm_vol[regs[0x36] & 0x0F]));
    opl2_write(0x51, (u8b)((ksl_map[patches[17].MKS] << 6) | fm_vol[(regs[0x37] >> 4) & 0x0F]));
    opl2_write(0x54, (u8b)((ksl_map[patches[17].CKS] << 6) | fm_vol[regs[0x37] & 0x0F]));
    opl2_write(0x52, (u8b)((ksl_map[patches[18].MKS] << 6) | fm_vol[(regs[0x38] >> 4) & 0x0F]));
    opl2_write(0x55, (u8b)((ksl_map[patches[18].CKS] << 6) | fm_vol[regs[0x38] & 0x0F]));
}

void ym2413opl_init(unsigned port)
{
    int i;
    opl2_init(port);
    opl3_write_bank1(0x05, 0x00);
    opl3_write_bank1(0x04, 0x00);
    memset(regs, 0, sizeof(regs));
    for (i = 0; i < 19; i++) decode_patch(&patches[i], ym2413_rom[i]);
    opl2_write(0x01, 0x20);
    for (i = 0; i < 9; i++) { vcref[i] = 0xFF; vlref[i] = 0; fref[i] = 0; }
    opl2_write(0xBD, 0xC0);
    for (i = 0; i < 3; i++) {
        opl2_write((u8b)(0x20 + i), 0x01); opl2_write((u8b)(0x23 + i), 0x01);
        opl2_write((u8b)(0x40 + i), 0x3F); opl2_write((u8b)(0x43 + i), 0x3F);
        opl2_write((u8b)(0x60 + i), 0xF0); opl2_write((u8b)(0x63 + i), 0xF0);
        opl2_write((u8b)(0x80 + i), 0xFF); opl2_write((u8b)(0x83 + i), 0xFF);
        opl2_write((u8b)(0xC0 + i), 0x30);
        opl2_write((u8b)(0x28 + i), 0x01); opl2_write((u8b)(0x2B + i), 0x01);
        opl2_write((u8b)(0x48 + i), 0x3F); opl2_write((u8b)(0x4B + i), 0x3F);
        opl2_write((u8b)(0x68 + i), 0xF0); opl2_write((u8b)(0x6B + i), 0xF0);
        opl2_write((u8b)(0x88 + i), 0xFF); opl2_write((u8b)(0x8B + i), 0xFF);
        opl2_write((u8b)(0xC3 + i), 0x30);
        opl2_write((u8b)(0x30 + i), 0x21); opl2_write((u8b)(0x33 + i), 0x21);
        opl2_write((u8b)(0x50 + i), 0x3F); opl2_write((u8b)(0x53 + i), 0x3F);
        opl2_write((u8b)(0x70 + i), 0xF0); opl2_write((u8b)(0x73 + i), 0xF0);
        opl2_write((u8b)(0x90 + i), 0xF0); opl2_write((u8b)(0x93 + i), 0xF0);
        opl2_write((u8b)(0xC6 + i), 0x30);
    }
}

void ym2413opl_silence(void)
{
    int ch;
    for (ch = 0; ch < 9; ch++)
        opl2_write((u8b)(0xB0 + ch), 0x00);
    opl2_write(0xBD, 0x00);
}

void ym2413opl_write(unsigned char addr, unsigned char data)
{
    u8b prev;
    int F, c;
    u16b freq;
    OplPatch *p = &patches[0];
    if (addr >= 64) return;
    prev = regs[addr];
    regs[addr] = data;
    switch (addr) {
        case 0x00: case 0x01: case 0x02: case 0x03:
        case 0x04: case 0x05: case 0x06: case 0x07:
            decode_patch(p, regs);
            set_user_voice();
            return;
        case 0x0E:
            if (RHYTHM_ON && !(prev & 0x20)) {
                rhythm_op(0x10, &patches[16], 0);
                rhythm_op(0x13, &patches[16], 1);
                rhythm_op(0x11, &patches[17], 0);
                rhythm_op(0x14, &patches[17], 1);
                rhythm_op(0x12, &patches[18], 0);
                rhythm_op(0x15, &patches[18], 1);
                opl2_write(0xC6, (u8b)((patches[16].FB << 1) | 0x30));
                opl2_write(0xC7, 0x30);
                opl2_write(0xC8, 0x30);
                for (c = 6; c < 9; c++) vcref[c] = 0xFF;
                rhythm_vols();
                opl2_write(0xA6, fref[6]&0xFF); opl2_write(0xB6, (u8b)((fref[6]>>8)&0x1F));
                opl2_write(0xA7, fref[7]&0xFF); opl2_write(0xB7, (u8b)((fref[7]>>8)&0x1F));
                opl2_write(0xA8, fref[8]&0xFF); opl2_write(0xB8, (u8b)((fref[8]>>8)&0x1F));
            }
            opl2_write(0xBD, (u8b)((data & 0x3F) | 0xC0));
            return;
        default: break;
    }
    if (addr >= 0x10 && addr <= 0x28) {
        F = addr & 0x0F;
        if (F > 8) return;
        freq = (u16b)((((u16b)regs[0x10+F] & 0xFF) | (((u16b)regs[0x20+F] & 1) << 8)) << 1);
        c = (regs[0x20+F] >> 1) & 7;
        fref[F] = (u16b)(freq | (c << 10) | ((regs[0x20+F]&0x10) ? 0x2000 : 0));
        opl2_write((u8b)(0xA0+F), (u8b)(fref[F] & 0xFF));
        opl2_write((u8b)(0xB0+F), (u8b)((fref[F] >> 8) & 0xFF));
        return;
    }
    if (addr >= 0x30 && addr < 0x36) {
        set_voice(addr & 0x0F, (data >> 4) & 0x0F, fm_vol[data & 0x0F]);
        return;
    }
    if (addr >= 0x36 && addr <= 0x38) {
        if (!RHYTHM_ON) {
            set_voice(addr & 0x0F, (data >> 4) & 0x0F, fm_vol[data & 0x0F]);
            return;
        }
        rhythm_vols();
    }
}
