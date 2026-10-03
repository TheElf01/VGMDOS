#ifndef SBDETECT_H
#define SBDETECT_H

typedef struct {
    int found;
    unsigned base_port;
    int irq;
    int dma8;
    int dma16;
    unsigned mpu_port;
    int type;
    unsigned opl_port;
    unsigned emu_port;
} SbInfo;

int sb_detect(SbInfo *info);

#endif
