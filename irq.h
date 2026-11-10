#ifndef IRQ_H
#define IRQ_H

int irq_install(int irq_num, unsigned sb_base);

void irq_remove(void);

unsigned long irq_get_count(void);

#endif
