#ifndef DMA_H
#define DMA_H

void dma_start_playback(int channel, unsigned long linear_addr,
                         unsigned length, int auto_init);

unsigned dma_get_count(int channel);

void dma_stop(int channel);

#endif
