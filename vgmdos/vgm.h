#ifndef VGM_H
#define VGM_H

#include <stdio.h>

#define VGM_DEFAULT_LOOPS 1

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned long  u32;

typedef struct {
    u32 eof_offset;
    u32 version;
    u32 sn76489_clock;
    u32 ym2413_clock;
    u32 ym2612_clock;
    unsigned dac_vol256;
    unsigned sn_vol256;
    u32 gd3_offset;
    u32 total_samples;
    u32 loop_offset;
    u32 loop_samples;
    u32 data_offset;
    int sn76489_dual_chip;
    u32 ay8910_clock;
    u32 nes_apu_clock;
    int has_fds;
    u32 gb_apu_clock;
    u32 ym2203_clock;
    u32 wswan_clock;
    u32 ym2608_clock;
    u32 pce_clock;
    u8 ay8910_type;
    u32 rf5c164_clock;
    u32 k051649_clock;
    u32 ym3812_clock;
    u32 ym3526_clock;
    u32 y8950_clock;
    u32 ymf262_clock;
} VgmHeader;

#define VGM_TAG_MAXLEN 64
#define VGM_NOTES_MAXLEN 512
typedef struct {
    int has_gd3;
    char track_name[VGM_TAG_MAXLEN];
    char game_name[VGM_TAG_MAXLEN];
    char author_name[VGM_TAG_MAXLEN];
    char system_name[VGM_TAG_MAXLEN];
    char release_date[VGM_TAG_MAXLEN];
    char vgm_by[VGM_TAG_MAXLEN];
    char notes[VGM_NOTES_MAXLEN];
} VgmTag;

typedef struct {
    FILE      *fp;
    unsigned char rbuf[512];
    unsigned   rb_pos, rb_len;
    long       rb_base;
    VgmHeader  header;
    long       data_start;
    long       loop_start;
    int        loop_count;
    int        max_loops;
    int        lz;
    long       lz_comp_start;
    long       lz_out;
    unsigned   lz_flags;
    unsigned   lz_mlen, lz_mdist, lz_r;
    int        lz_ck_ok;
    long       lz_ck_file;
    unsigned   lz_ck_flags, lz_ck_mlen, lz_ck_mdist, lz_ck_r;
    FILE      *pcm_fp;
    long       pcm_bank_offset;
    u32        pcm_bank_size;
    long       vdac_offset;
    u32        pcm_read_pos;
    u32        inline_left;
    unsigned   rf_dst;
    int        skip_pcm;
} VgmFile;

enum {
    VGM_CMD_PSG2_WRITE     = 0x30,
    VGM_CMD_GG_STEREO      = 0x4F,
    VGM_CMD_PSG_WRITE      = 0x50,
    VGM_CMD_YM2413_WRITE   = 0x51,
    VGM_CMD_YM2612_PORT0   = 0x52,
    VGM_CMD_YM2612_PORT1   = 0x53,
    VGM_CMD_YM2203_WRITE   = 0x55,
    VGM_CMD_YM2608_PORT0   = 0x56,
    VGM_CMD_YM2608_PORT1   = 0x57,
    VGM_CMD_YM3812_WRITE   = 0x5A,
    VGM_CMD_YM3526_WRITE   = 0x5B,
    VGM_CMD_Y8950_WRITE    = 0x5C,
    VGM_CMD_YMF262_PORT0   = 0x5E,
    VGM_CMD_YMF262_PORT1   = 0x5F,
    VGM_CMD_WSWAN_PORT     = 0xBC,
    VGM_CMD_WSWAN_MEM      = 0xC6,
    VGM_CMD_AY8910_WRITE   = 0xA0,
    VGM_CMD_GB_APU_WRITE   = 0xB3,
    VGM_CMD_NES_APU_WRITE  = 0xB4,
    VGM_CMD_PCE_WRITE      = 0xB9,
    VGM_CMD_WAIT_N16       = 0x61,
    VGM_CMD_WAIT_735       = 0x62,
    VGM_CMD_WAIT_882       = 0x63,
    VGM_CMD_END            = 0x66,
    VGM_CMD_DATA_BLOCK     = 0x67,
    VGM_CMD_WAIT_SMALL_LO  = 0x70,
    VGM_CMD_WAIT_SMALL_HI  = 0x7F,
    VGM_CMD_YM2612_PCM_LO  = 0x80,
    VGM_CMD_YM2612_PCM_HI  = 0x8F,
    VGM_CMD_PCM_SEEK       = 0xE0
};

typedef enum {
    VGM_EV_PSG_WRITE = 0,
    VGM_EV_PSG2_WRITE,
    VGM_EV_AY8910_WRITE,
    VGM_EV_SCC_WRITE,
    VGM_EV_NES_APU_WRITE,
    VGM_EV_PCE_WRITE,
    VGM_EV_GB_APU_WRITE,
    VGM_EV_YM2413_WRITE,
    VGM_EV_YM2203_WRITE,
    VGM_EV_YM2608_WRITE,
    VGM_EV_YM3812_WRITE,
    VGM_EV_YM3526_WRITE,
    VGM_EV_Y8950_WRITE,
    VGM_EV_YMF262_WRITE,
    VGM_EV_WSWAN_PORT,
    VGM_EV_WSWAN_MEM,
    VGM_EV_YM2612_WRITE,
    VGM_EV_PCM_BANK,
    VGM_EV_DAC_TABLE,
    VGM_EV_NES_RAM,
    VGM_EV_NES_PCM,
    VGM_EV_DAC_FREQ,
    VGM_EV_DAC_START,
    VGM_EV_DAC_STOP,
    VGM_EV_RF5C_WRITE,
    VGM_EV_RF5C_MEM,
    VGM_EV_RF5C_RAM,
    VGM_EV_RF5C_BANK,
    VGM_EV_RF5C_COPY,
    VGM_EV_RF5C_TABLE,
    VGM_EV_AWE_REG,
    VGM_EV_AWE_RING,
    VGM_EV_WAIT,
    VGM_EV_END,
    VGM_EV_UNKNOWN
} VgmEventType;

typedef struct {
    VgmEventType type;
    u32 addr;
    u8  data;
    u32 wait;
} VgmEvent;

unsigned vgm_read_inline(VgmFile *vgm, unsigned char *dst, unsigned n);

int vgm_open(VgmFile *vgm, const char *path);

unsigned long vgm_estimated_total_samples(const VgmHeader *hdr, int max_loops);

void vgm_close(VgmFile *vgm);

int vgm_next_event(VgmFile *vgm, VgmEvent *ev);

void vgm_prewarm(VgmFile *vgm);

int vgm_read_gd3(VgmFile *vgm, VgmTag *tag);

#endif
