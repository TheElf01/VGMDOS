#ifndef DOSMEM_H
#define DOSMEM_H

#include <dos.h>

typedef struct {
    unsigned int dos_segment;
    unsigned char far *ptr;
    unsigned long linear_addr;
    unsigned size;
} DosBuffer;

int dosmem_alloc(DosBuffer *b, unsigned size);

int dosmem_alloc_simple(DosBuffer *b, unsigned size);

void dosmem_free(DosBuffer *b);

#endif
