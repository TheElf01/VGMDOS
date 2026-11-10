#include <conio.h>
#include "dma.h"

typedef struct { unsigned addr_port, count_port, page_port; } DmaPorts;

static const DmaPorts dma_ports[4] = {
    { 0x00, 0x01, 0x87 },
    { 0x02, 0x03, 0x83 },
    { 0x04, 0x05, 0x81 },
    { 0x06, 0x07, 0x82 },
};

static const DmaPorts dma16_ports[3] = {
    { 0xC4, 0xC6, 0x8B },
    { 0xC8, 0xCA, 0x89 },
    { 0xCC, 0xCE, 0x8A },
};

static void dma16_start(int channel, unsigned long linear_addr, unsigned words, int auto_init)
{
    const DmaPorts *p = &dma16_ports[(channel - 5) % 3];
    unsigned offset = (unsigned)((linear_addr >> 1) & 0xFFFFUL);
    unsigned char page = (unsigned char)((linear_addr >> 16) & 0xFE);
    unsigned count = words - 1;
    int ch = channel & 3;
    outp(0xD4, (unsigned char)(0x04 | ch));
    outp(0xD8, 0x00);
    outp(0xD6, (unsigned char)(0x48 | ch | (auto_init ? 0x10 : 0)));
    outp(p->addr_port, (unsigned char)(offset & 0xFF));
    outp(p->addr_port, (unsigned char)((offset >> 8) & 0xFF));
    outp(p->count_port, (unsigned char)(count & 0xFF));
    outp(p->count_port, (unsigned char)((count >> 8) & 0xFF));
    outp(p->page_port, page);
    outp(0xD4, (unsigned char)ch);
}

void dma_start_playback(int channel, unsigned long linear_addr,
                         unsigned length, int auto_init)
{
    const DmaPorts *p = &dma_ports[channel & 0x03];
    unsigned offset = (unsigned)(linear_addr & 0xFFFFUL);
    unsigned char page = (unsigned char)((linear_addr >> 16) & 0xFF);
    unsigned count = length - 1;
    unsigned char mode;
    if (channel >= 4) { dma16_start(channel, linear_addr, length, auto_init); return; }
    outp(0x0A, (unsigned char)(0x04 | channel));
    outp(0x0C, 0x00);
    mode = (unsigned char)(0x48 | channel);
    if (auto_init)
        mode |= 0x10;
    outp(0x0B, mode);
    outp(p->addr_port, (unsigned char)(offset & 0xFF));
    outp(p->addr_port, (unsigned char)((offset >> 8) & 0xFF));
    outp(p->count_port, (unsigned char)(count & 0xFF));
    outp(p->count_port, (unsigned char)((count >> 8) & 0xFF));
    outp(p->page_port, page);
    outp(0x0A, (unsigned char)channel);
}

void dma_stop(int channel)
{
    if (channel >= 4) { outp(0xD4, (unsigned char)(0x04 | (channel & 3))); return; }
    outp(0x0A, (unsigned char)(0x04 | channel));
}

unsigned dma_get_count(int channel)
{
    const DmaPorts *p = (channel >= 4) ? &dma16_ports[(channel - 5) % 3] : &dma_ports[channel & 0x03];
    unsigned ff = (channel >= 4) ? 0xD8 : 0x0C;
    unsigned a, b;
    int tries;
    b = 0;
    for (tries = 0; tries < 4; tries++) {
        outp(ff, 0x00);
        a = (unsigned)inp(p->count_port);
        a |= (unsigned)inp(p->count_port) << 8;
        outp(ff, 0x00);
        b = (unsigned)inp(p->count_port);
        b |= (unsigned)inp(p->count_port) << 8;
        if ((a > b ? a - b : b - a) < 16) break;
    }
    return b;
}
