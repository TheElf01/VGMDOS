#include "SNES_SPC.h"
#include "spcemu.h"
#include <stdlib.h>

void *spce_open(const unsigned char *spc, long n)
{
    SNES_SPC *s = new SNES_SPC;
    if (!s) return 0;
    if (s->init() || s->load_spc(spc, n)) { delete s; return 0; }
    s->clear_echo();
    if (getenv("SPCMUTE")) s->mute_voices(atoi(getenv("SPCMUTE")));
    return s;
}
void spce_close(void *e) { delete (SNES_SPC *)e; }
void spce_run(void *e, int nsamples, short *out)
{
    static short tmp[4096];
    SNES_SPC *s = (SNES_SPC *)e;
    while (nsamples > 0) {
        int k = nsamples > 2048 ? 2048 : nsamples;
        s->play(k * 2, out ? out : tmp);
        if (out) out += k * 2;
        nsamples -= k;
    }
}
void spce_voice(void *e, int i, SpcVoice *v)
{
    SPC_DSP const *d = ((SNES_SPC *)e)->x_dsp();
    SPC_DSP::voice_t const *x = d->x_voice(i);
    v->kon = x->x_kon; v->start = x->x_start; v->dira = x->x_dira;
    v->env = x->env; v->release = (x->env_mode == SPC_DSP::env_release);
    v->pitch = x->x_pitch; v->non = x->x_non ? 1 : 0;
    v->voll = (signed char)x->regs[0]; v->volr = (signed char)x->regs[1];
}
const unsigned char *spce_ram(void *e) { return ((SNES_SPC *)e)->x_dsp()->x_ram(); }
int spce_reg(void *e, int r) { return ((SNES_SPC *)e)->x_dsp()->x_regs()[r & 0x7F]; }
