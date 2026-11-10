#include <conio.h>
#include "pgus_tandy.h"

static unsigned g_port = 0xC0;

void pgus_tandy_init(unsigned port)
{
    g_port = port ? port : 0xC0;
}

void pgus_tandy_write(unsigned char data)
{
    outp(g_port, data);
}

void pgus_tandy_silence(void)
{
    outp(g_port, 0x9F);
    outp(g_port, 0xBF);
    outp(g_port, 0xDF);
    outp(g_port, 0xFF);
}
