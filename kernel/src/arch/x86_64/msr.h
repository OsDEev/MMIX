#ifndef MYUNIX_ARCH_MSR_H
#define MYUNIX_ARCH_MSR_H

#include <stdint.h>

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(msr));
}

/* Debug helpers for the page-fault dump. */
static inline uint64_t debug_gs_base(void) {
    return rdmsr(0xC0000101); /* IA32_GS_BASE */
}

static inline uint64_t debug_kgs_base(void) {
    return rdmsr(0xC0000102); /* IA32_KERNEL_GS_BASE */
}

#endif /* MYUNIX_ARCH_MSR_H */
