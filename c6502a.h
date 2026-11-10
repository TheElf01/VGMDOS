#ifndef C6502A_H
#define C6502A_H

#ifndef C6U16
#define C6U16 unsigned
#endif

typedef struct {
    unsigned char a, x, y, s;
    unsigned char fl;
    unsigned char fv, fd, fi;
    C6U16 pc;
    C6U16 ramseg;
    C6U16 ticks;
    C6U16 io_a;
    unsigned char io_v;
    unsigned char status;
} C6;

unsigned c6_run(C6 *p);

#endif
