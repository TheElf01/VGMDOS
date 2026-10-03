#include <string.h>
#include "ym2203_opl.h"
#include "opl2.h"
#include "fastmath.h"

static const int op1_offset[9] = { 0x00,0x01,0x02, 0x08,0x09,0x0A, 0x10,0x11,0x12 };
static const int op2_offset[9] = { 0x03,0x04,0x05, 0x0B,0x0C,0x0D, 0x13,0x14,0x15 };
static const unsigned char slot_to_op[4] = { 0, 2, 1, 3 };

typedef struct {
    unsigned fnum, block;
    unsigned algorithm, feedback;
    unsigned keyon;
    unsigned char op_mul[4];
    unsigned char op_dt[4];
    unsigned char op_tl[4];
    unsigned char op_ar[4];
    unsigned char op_ks[4];
    unsigned char op_d1r[4];
    unsigned char op_am[4];
    unsigned char op_d2r[4];
    unsigned char op_sl[4];
    unsigned char op_rr[4];
} OpnChannel;

static OpnChannel chan[6];
static unsigned long g_ym_clock = 3993600UL;
static int g_num_ch = 3;

static int opl_op_offset(int chanA, int chanB, int op_idx)
{
    switch (op_idx) {
        case 0: return op1_offset[chanA];
        case 1: return op2_offset[chanA];
        case 2: return op1_offset[chanB];
        default: return op2_offset[chanB];
    }
}

static void write_reg(int bank, unsigned char reg, unsigned char data)
{
    if (bank) opl3_write_bank1_fast(reg, data);
    else opl3_write_bank0_fast(reg, data);
}

static unsigned long ym_fnum_block_to_hzQ8(unsigned fnum, unsigned block)
{
    unsigned int clock16 = (unsigned int)(g_ym_clock >> 8);
    unsigned long num = fast_mul16(fnum, clock16);
    {
        int e = 5 - (int)block;
        unsigned long divisor = (e >= 0) ? (144UL << e) : (144UL >> (-e));
        return num / divisor;
    }
}

static void opl_freq_from_hzQ8(unsigned long hzq8, unsigned *out_fnum, unsigned *out_block)
{
    int block;
    unsigned long f = 0;
    for (block = 0; block <= 7; block++) {
        int e = 12 - block;
        unsigned long limit = 0xFFFFFFFFUL >> e;
        if (hzq8 > limit) continue;
        f = (hzq8 << e) / 49716UL;
        if (f <= 1023UL) break;
    }
    if (block > 7) { block = 7; f = 1023UL; }
    if (f > 1023UL) f = 1023UL;
    *out_fnum = (unsigned)f;
    *out_block = (unsigned)block;
}

static void push_freq(int ch)
{
    int bank = (ch < 3) ? 0 : 1;
    int chanA = ch % 3;
    OpnChannel *c = &chan[ch];
    unsigned long hzq8 = ym_fnum_block_to_hzQ8(c->fnum, c->block);
    unsigned opl_fnum, opl_block;
    unsigned char b0;
    opl_freq_from_hzQ8(hzq8, &opl_fnum, &opl_block);
    write_reg(bank, (unsigned char)(0xA0 + chanA), (unsigned char)(opl_fnum & 0xFF));
    b0 = (unsigned char)((c->keyon ? 0x20 : 0x00) | (opl_block << 2) | ((opl_fnum >> 8) & 0x03));
    write_reg(bank, (unsigned char)(0xB0 + chanA), b0);
}

static void push_connection(int ch)
{
    int bank = (ch < 3) ? 0 : 1;
    int chanA = ch % 3, chanB = chanA + 3;
    OpnChannel *c = &chan[ch];
    unsigned cnt_a, cnt_b;
    unsigned char val_a, val_b;
    switch (c->algorithm) {
        case 0: cnt_a = 0; cnt_b = 0; break;
        case 1: case 2: cnt_a = 1; cnt_b = 0; break;
        case 3: case 4: cnt_a = 0; cnt_b = 1; break;
        default: cnt_a = 1; cnt_b = 1; break;
    }
    val_a = (unsigned char)(0x30 | ((c->feedback & 0x07) << 1) | cnt_a);
    val_b = (unsigned char)(0x30 | cnt_b);
    write_reg(bank, (unsigned char)(0xC0 + chanA), val_a);
    write_reg(bank, (unsigned char)(0xC0 + chanB), val_b);
}

static void push_operator(int ch, int op_idx)
{
    int bank = (ch < 3) ? 0 : 1;
    int chanA = ch % 3, chanB = chanA + 3;
    int off = opl_op_offset(chanA, chanB, op_idx);
    OpnChannel *c = &chan[ch];
    unsigned char tl, ar, dr, sl, rr;
    unsigned egt;
    tl = (c->op_tl[op_idx] > 63) ? 63 : c->op_tl[op_idx];
    ar = (unsigned char)(c->op_ar[op_idx] >> 1);
    dr = (unsigned char)(c->op_d1r[op_idx] >> 1);
    sl = c->op_sl[op_idx];
    if (c->op_d2r[op_idx] >= 10) {
        egt = 0;
        rr = (unsigned char)(c->op_d2r[op_idx] >> 2);
    } else {
        egt = 1;
        rr = c->op_rr[op_idx];
    }
    write_reg(bank, (unsigned char)(0x20 + off),
        (unsigned char)((c->op_am[op_idx] ? 0x80 : 0)
                       | (egt << 5)
                       | ((c->op_ks[op_idx] != 0) ? 0x10 : 0)
                       | (c->op_mul[op_idx] & 0x0F)));
    write_reg(bank, (unsigned char)(0x40 + off), tl);
    write_reg(bank, (unsigned char)(0x60 + off), (unsigned char)((ar << 4) | dr));
    write_reg(bank, (unsigned char)(0x80 + off), (unsigned char)((sl << 4) | rr));
}

void ym2203opl_init(unsigned port, unsigned long clock, int num_channels)
{
    int ch;
    g_num_ch = (num_channels == 6) ? 6 : 3;
    opl2_init(port);
    memset(chan, 0, sizeof(chan));
    g_ym_clock = clock ? clock : ((g_num_ch == 6) ? 8000000UL : 3993600UL);
    opl3_write_bank1_fast(0x05, 0x01);
    opl3_write_bank1_fast(0x04, (g_num_ch == 6) ? 0x3F : 0x07);
    opl3_write_bank0_fast(0xBD, 0x00);
    for (ch = 0; ch < g_num_ch; ch++) {
        int bank = (ch < 3) ? 0 : 1;
        int chanA = ch % 3, chanB = chanA + 3;
        int op;
        for (op = 0; op < 4; op++) {
            int off = opl_op_offset(chanA, chanB, op);
            write_reg(bank, (unsigned char)(0x20 + off), 0x01);
            write_reg(bank, (unsigned char)(0x40 + off), 0x3F);
            write_reg(bank, (unsigned char)(0x60 + off), 0xF4);
            write_reg(bank, (unsigned char)(0x80 + off), 0x0F);
            write_reg(bank, (unsigned char)(0xE0 + off), 0x00);
        }
        write_reg(bank, (unsigned char)(0xC0 + chanA), 0x30);
        write_reg(bank, (unsigned char)(0xC0 + chanB), 0x30);
    }
}

void ym2203opl_silence(void)
{
    int i;
    for (i = 0; i < 3; i++) {
        opl3_write_bank0_fast((unsigned char)(0xB0 + i), 0x00);
        if (g_num_ch == 6)
            opl3_write_bank1_fast((unsigned char)(0xB0 + i), 0x00);
    }
    opl3_write_bank1_fast(0x04, 0x00);
    opl3_write_bank1_fast(0x05, 0x00);
}

void ym2203opl_write(int port, unsigned char addr, unsigned char data)
{
    int ch_base = (port == 0) ? 0 : 3;
    if (port != 0 && g_num_ch != 6) return;
    if (addr == 0x28) {
        unsigned raw_ch = data & 0x07;
        unsigned ch_idx;
        if (raw_ch == 3 || raw_ch == 7) return;
        ch_idx = (raw_ch < 4) ? raw_ch : (raw_ch - 4 + 3);
        if (ch_idx >= (unsigned)g_num_ch) return;
        chan[ch_idx].keyon = ((data & 0xF0) != 0) ? 1 : 0;
        push_freq((int)ch_idx);
        return;
    }
    if (addr >= 0x30 && addr <= 0x9F) {
        unsigned group = addr & 0xF0;
        unsigned slot = (addr >> 2) & 0x03;
        unsigned ch_in_group = addr & 0x03;
        unsigned ch_idx;
        int op_idx;
        if (ch_in_group == 3) return;
        ch_idx = (unsigned)ch_base + ch_in_group;
        if (ch_idx >= (unsigned)g_num_ch) return;
        op_idx = slot_to_op[slot];
        {
            OpnChannel *c = &chan[ch_idx];
            switch (group) {
                case 0x30: c->op_mul[op_idx] = data & 0x0F; c->op_dt[op_idx] = (unsigned char)((data >> 4) & 0x07); break;
                case 0x40: c->op_tl[op_idx] = data & 0x7F; break;
                case 0x50: c->op_ar[op_idx] = data & 0x1F; c->op_ks[op_idx] = (unsigned char)((data >> 6) & 0x03); break;
                case 0x60: c->op_d1r[op_idx] = data & 0x1F; c->op_am[op_idx] = (unsigned char)((data & 0x80) ? 1 : 0); break;
                case 0x70: c->op_d2r[op_idx] = data & 0x1F; break;
                case 0x80: c->op_sl[op_idx] = (unsigned char)((data >> 4) & 0x0F); c->op_rr[op_idx] = data & 0x0F; break;
                default: return;
            }
            push_operator((int)ch_idx, op_idx);
        }
        return;
    }
    if (addr >= 0xA0 && addr <= 0xA2) {
        unsigned ch_idx = (unsigned)ch_base + (addr - 0xA0);
        if (ch_idx >= (unsigned)g_num_ch) return;
        chan[ch_idx].fnum = (chan[ch_idx].fnum & 0x700) | data;
        push_freq((int)ch_idx);
        return;
    }
    if (addr >= 0xA4 && addr <= 0xA6) {
        unsigned ch_idx = (unsigned)ch_base + (addr - 0xA4);
        if (ch_idx >= (unsigned)g_num_ch) return;
        chan[ch_idx].block = (data >> 3) & 0x07;
        chan[ch_idx].fnum = (chan[ch_idx].fnum & 0x0FF) | ((unsigned)(data & 0x07) << 8);
        push_freq((int)ch_idx);
        return;
    }
    if (addr >= 0xB0 && addr <= 0xB2) {
        unsigned ch_idx = (unsigned)ch_base + (addr - 0xB0);
        if (ch_idx >= (unsigned)g_num_ch) return;
        chan[ch_idx].algorithm = data & 0x07;
        chan[ch_idx].feedback = (data >> 3) & 0x07;
        push_connection((int)ch_idx);
        return;
    }
}
