#include <conio.h>
#include <dos.h>
#include "irq.h"

static volatile unsigned long g_irq_count = 0;
static unsigned g_sb_base = 0;
static int g_vector = 0;
static int g_irq_num = 0;
static void (__interrupt __far *g_old_vector)(void) = 0;
static int g_installed = 0;

static void __interrupt __far irq_isr(void)
{
    inp(g_sb_base + 0x0E);
    inp(g_sb_base + 0x0F);
    g_irq_count++;
    if (g_irq_num >= 8)
        outp(0xA0, 0x20);
    outp(0x20, 0x20);
}

int irq_install(int irq_num, unsigned sb_base)
{
    if (irq_num < 0 || irq_num > 15) return 0;
    g_sb_base = sb_base;
    g_irq_num = irq_num;
    g_vector = (irq_num < 8) ? (0x08 + irq_num) : (0x70 + (irq_num - 8));
    g_old_vector = _dos_getvect(g_vector);
    _dos_setvect(g_vector, irq_isr);
    if (irq_num < 8)
        outp(0x21, (unsigned char)(inp(0x21) & ~(1 << irq_num)));
    else
        outp(0xA1, (unsigned char)(inp(0xA1) & ~(1 << (irq_num - 8))));
    g_installed = 1;
    return 1;
}

void irq_remove(void)
{
    if (g_installed) {
        _dos_setvect(g_vector, g_old_vector);
        g_installed = 0;
    }
}

unsigned long irq_get_count(void)
{
    return g_irq_count;
}
