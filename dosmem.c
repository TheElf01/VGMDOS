#include <stdio.h>
#include <string.h>
#include "dosmem.h"

static int dosmem_alloc_ex(DosBuffer *b, unsigned size, unsigned long margin)
{
    unsigned paragraphs;
    unsigned seg;
    unsigned long raw_linear;
    unsigned err;
    memset(b, 0, sizeof(*b));
    b->size = size;
    paragraphs = (unsigned)(((unsigned long)size + margin + 15UL) / 16UL);
    err = _dos_allocmem(paragraphs, &seg);
    if (err != 0) {
        fprintf(stderr, "dosmem_alloc: _dos_allocmem failed (code %u), "
                        "paragraphs=%u\n", err, paragraphs);
        return 0;
    }
    b->dos_segment = seg;
    raw_linear = (unsigned long)seg * 16UL;
    b->linear_addr = margin ? ((raw_linear + 65535UL) & ~65535UL) : raw_linear;
#ifdef __386__
    b->ptr = (unsigned char *)b->linear_addr;
#else
    b->ptr = (unsigned char far *)MK_FP((unsigned)(b->linear_addr >> 4), 0);
#endif
    return 1;
}

int dosmem_alloc(DosBuffer *b, unsigned size)
{
    return dosmem_alloc_ex(b, size, 65535UL);
}

int dosmem_alloc_simple(DosBuffer *b, unsigned size)
{
    return dosmem_alloc_ex(b, size, 0UL);
}

void dosmem_free(DosBuffer *b)
{
    if (b->dos_segment)
        _dos_freemem(b->dos_segment);
    memset(b, 0, sizeof(*b));
}
