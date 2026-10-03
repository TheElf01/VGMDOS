#ifndef SPCEMU_H
#define SPCEMU_H
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    long kon;
    int start;
    int dira;
    int env;
    int release;
    int pitch;
    int non;
    int voll, volr;
} SpcVoice;
void *spce_open(const unsigned char *spc, long n);
void spce_close(void *e);
void spce_run(void *e, int nsamples, short *out);
void spce_voice(void *e, int i, SpcVoice *v);
const unsigned char *spce_ram(void *e);
int spce_reg(void *e, int r);
#ifdef __cplusplus
}
#endif
#endif
