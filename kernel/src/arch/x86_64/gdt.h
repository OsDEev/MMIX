#ifndef MYUNIX_GDT_H
#define MYUNIX_GDT_H

#include <stdint.h>

/* Segment selectors */
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE   0x18
#define GDT_USER_DATA   0x20
#define GDT_TSS         0x28 /* spans entries 5-6 (16-byte descriptor) */

void gdt_init(void);

/* Update the ring-0 stack pointer used on interrupts from user mode. */
void tss_set_rsp0(uint64_t rsp0);

#endif /* MYUNIX_GDT_H */
