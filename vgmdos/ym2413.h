#ifndef YM2413_H
#define YM2413_H

typedef struct YM2413 {
    int unused;
} YM2413;

#define YM2413_PATCHSET_GENERIC 0
#define YM2413_PATCHSET_MSX     1

void ym2413_init(YM2413 *c, unsigned long clock, unsigned long sample_rate, int patch_set);
void ym2413_write(YM2413 *c, unsigned char addr, unsigned char data);
void ym2413_run(YM2413 *c, short *out, unsigned long n);

#endif
