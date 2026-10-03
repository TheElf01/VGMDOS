#include <conio.h>
#include "pctimer.h"

#ifdef __386__

#define BIOS_TICKS (*(volatile unsigned long *)0x46CUL)
#else

#include <dos.h>
#define BIOS_TICKS (*(volatile unsigned long far *)MK_FP(0x40, 0x6C))
#endif

unsigned long pctimer_now(void)
{
    static unsigned long last_raw = 0, total = 0;
    static int init = 0;
    unsigned long bios1, bios2, raw, delta;
    unsigned char lo, hi;
    unsigned countdown;
    do {
        bios1 = BIOS_TICKS;
        outp(0x43, 0x00);
        lo = (unsigned char)inp(0x40);
        hi = (unsigned char)inp(0x40);
        countdown = (unsigned)lo | ((unsigned)hi << 8);
        bios2 = BIOS_TICKS;
    } while (bios1 != bios2);
    raw = bios1 * 65536UL + (65536UL - countdown);
    if (!init) { init = 1; last_raw = raw; }
    delta = raw - last_raw;
    if (delta < 0x80000000UL) { total += delta; last_raw = raw; }
    else if (last_raw - raw > 0x10000000UL) last_raw = raw;
    return total;
}
