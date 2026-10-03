#include "ym2413.h"

void ym2413_init(YM2413 *c, unsigned long clock, unsigned long sample_rate, int patch_set)
{
    (void)c; (void)clock; (void)sample_rate; (void)patch_set;
}

void ym2413_write(YM2413 *c, unsigned char addr, unsigned char data)
{
    (void)c; (void)addr; (void)data;
}

void ym2413_run(YM2413 *c, short *out, unsigned long n)
{
    (void)c; (void)out; (void)n;
}
