#ifndef MYUNIX_LAPIC_H
#define MYUNIX_LAPIC_H

#include <stdbool.h>
#include <stdint.h>

#define LAPIC_VA 0xffffffffb0000000ULL /* kernel-space MMIO alias */
#define TIMER_VECTOR 48                /* above the PIC range (32..47) */
#define TIMER_HZ 50

bool lapic_init(void);
bool lapic_active(void);
void lapic_eoi(void);

#endif /* MYUNIX_LAPIC_H */
