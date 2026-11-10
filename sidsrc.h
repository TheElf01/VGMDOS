#ifndef SIDSRC_H
#define SIDSRC_H

#include "vgm.h"

int sidsrc_open(VgmFile *v);

int sidsrc_next(VgmFile *v, VgmEvent *ev);

void sidsrc_close(void);

void sidsrc_tag(VgmFile *v, VgmTag *tag);

#endif
