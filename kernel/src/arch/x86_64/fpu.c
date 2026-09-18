/*
 * FPU/SSE initialization.
 *
 * The kernel and every userspace binary are built with
 * -mno-80387 -mno-mmx -mno-sse -mno-sse2, so no SSE/x87 instructions are
 * generated from C.  However the C runtime and hand-written assembly can
 * still execute MMX/SSE-style moves (e.g. through clang/ld.lld memcpy
 * stubs), and on *real* hardware the firmware or a previous stage may
 * leave CR0.EM (emulation) or CR0.TS (task switched) set - which makes
 * the first such instruction raise #NM (Device Not Available).  QEMU
 * tolerates this (its default state has CR0 clear), real machines do not.
 *
 * We therefore enable the hardware FPU/SSE explicitly and provide a #NM
 * rescue path (see fpu_nm_rescue in idt.c) that impatiently clears TS.
 */
#include <fpu.h>
#include <stdint.h>

void fpu_init(void) {
    uint64_t cr0, cr4;

    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    /* Clear EM (bit 2, "x87 EMIULATION"... emulation) and TS (bit 3). */
    cr0 &= ~((1ull << 2) | (1ull << 3));
    cr0 |= (1ull << 1);  /* MP: keep the monitor-coprocessor bit benign */
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));

    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    /* OSFXSR (bit 9) + OSXMMEXCPT (bit 10): SSE + #XM are OS-managed. */
    cr4 |= (1ull << 9) | (1ull << 10);
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));

    /* Initialize the x87 unit so the first runtime op sees a sane state. */
    __asm__ volatile("fninit" ::: "memory");

    /* Reset MXCSR to the architectural default: all SIMD exceptions
     * masked, round-to-nearest.  A dirty MXCSR left behind by a prior
     * stage could otherwise raise #XM on the first SSE-dependent op. */
    uint32_t mxcsr = 0x1F80u;
    __asm__ volatile("ldmxcsr %0" :: "m"(mxcsr) : "memory");
}