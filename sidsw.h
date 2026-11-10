#ifndef SIDSW_H
#define SIDSW_H

#ifndef SID_U32
#define SID_U32 unsigned long
#define SID_S32 long
#endif
typedef SID_U32 sid_u32;
typedef SID_S32 sid_s32;

typedef struct {
    sid_u32 acc[3];
    sid_u32 inc[3];
    sid_u32 lfsr[3];
    sid_u32 rate_cnt[3];
    unsigned rate_per[3];
    unsigned exp_cnt[3];
    unsigned char exp_per[3];
    unsigned char env[3];
    unsigned char state[3];
    unsigned char ctrl[3];
    unsigned pw[3];
    unsigned freq[3];
    unsigned char ad[3];
    unsigned char sr[3];
    unsigned char noise_out[3];
    unsigned char reg[32];
    sid_u32 clock;
    sid_u32 rate;
    sid_u32 cps_q8;
    unsigned blk_left;
    int lp, bp, hp;
    int w0;
    int q1;
    int nsub;
    unsigned gain;
    unsigned char ty[3];
} SidSw;

void sidsw_init(SidSw *s, unsigned long clock, unsigned long sample_rate);
void sidsw_write_reg(SidSw *s, unsigned reg, unsigned char data);
void sidsw_run(SidSw *s, unsigned char far *out, unsigned long count);

#endif
