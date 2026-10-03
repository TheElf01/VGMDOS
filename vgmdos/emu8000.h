#ifndef EMU8000_H
#define EMU8000_H

#define EMU8000_DRAM_START 0x200000UL

int emu8000_init(unsigned base_port);

int emu8000_load_sample(unsigned long addr, const short *data, unsigned long count);

void emu8000_note_on(int channel, unsigned long start, unsigned long end, unsigned int pitch, int volume);

void emu8000_note_off(int channel);

void emu8000_set_pitch(int channel, unsigned int pitch);

void emu8000_set_volume(int channel, int volume);

void emu8000_set_loop(int channel, unsigned long start, unsigned long end);


void emu8000_set_pan(unsigned char p);
void emu8000_set_filter(unsigned char f);
void emu8000_play(int channel, unsigned long start, unsigned long lstart, unsigned long lend,
                  unsigned int pitch, unsigned char atten);
void emu8000_set_atten(int channel, unsigned char atten);

#endif
