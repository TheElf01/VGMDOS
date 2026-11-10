#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sbdetect.h"
extern int g_en;

int sb_detect(SbInfo *info)
{
    const char *blaster;
    const char *p;
    memset(info, 0, sizeof(*info));
    info->opl_port = 0x388;
    blaster = getenv("BLASTER");
    if (!blaster) {
        if (g_en)
            fprintf(stderr, "BLASTER environment variable not found.\n"
                            "  Make sure your AUTOEXEC.BAT has something like:\n"
                            "  SET BLASTER=A220 I5 D1 H5 P330 T6\n");
        else
            fprintf(stderr, "No se encontro la variable de entorno BLASTER.\n"
                            "  Asegurate de que tu AUTOEXEC.BAT tiene algo como:\n"
                            "  SET BLASTER=A220 I5 D1 H5 P330 T6\n");
        return 0;
    }
    info->found = 1;
    for (p = blaster; *p; p++) {
        char c = *p;
        if (c == 'A' || c == 'a') {
            info->base_port = (unsigned)strtol(p + 1, NULL, 16);
        } else if (c == 'I' || c == 'i') {
            info->irq = atoi(p + 1);
        } else if (c == 'D' || c == 'd') {
            info->dma8 = atoi(p + 1);
        } else if (c == 'H' || c == 'h') {
            info->dma16 = atoi(p + 1);
        } else if (c == 'P' || c == 'p') {
            info->mpu_port = (unsigned)strtol(p + 1, NULL, 16);
        } else if (c == 'T' || c == 't') {
            info->type = atoi(p + 1);
        } else if (c == 'E' || c == 'e') {
            info->emu_port = (unsigned)strtol(p + 1, NULL, 16);
        }
    }
    return (info->base_port != 0);
}
