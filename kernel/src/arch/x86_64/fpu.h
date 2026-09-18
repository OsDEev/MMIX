/*
 * FPU/SSE enable + #NM (Device Not Available) rescue.
 *
 * The kernel and userspace are both compiled without x87/MMX/SSE, but a
 * few compiler-emitted routines can still dereference the FP state, and
 * on real hardware the firmware/bootloader may leave CR0.EM or CR0.TS
 * set.  We clear both at boot so any such instruction runs, and the ISR
 * handler below re-tries a faulting FP/SSE instruction once by clearing
 * CR0.TS (the standard lazy-switching fallback).
 */
#ifndef MYUNIX_FPU_H
#define MYUNIX_FPU_H

void fpu_init(void);

#endif /* MYUNIX_FPU_H */