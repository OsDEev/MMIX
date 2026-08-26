/*
 * Per-CPU data, reachable through GS in ring 0.
 *
 * Convention: IA32_KERNEL_GS_BASE holds &cpu_data; SYSCALL/SYSRET paths
 * use swapgs so that gs:CPU_KSTACK_TOP always resolves to the current
 * task's kernel stack top while executing in the kernel.
 */
#ifndef MYUNIX_CPU_H
#define MYUNIX_CPU_H

#include <stdint.h>

struct cpu_data {
    uint64_t kstack_top; /* 0x00: kernel stack top of current task */
    uint64_t user_rsp;   /* 0x08: user RSP saved by syscall entry */
    uint64_t user_rip;   /* 0x10 */
    uint64_t pad[5];
} __attribute__((aligned(64)));

#define CPU_KSTACK_TOP 0x00
#define CPU_USER_RSP   0x08
#define CPU_USER_RIP   0x10

extern struct cpu_data bsp_cpu;

#endif /* MYUNIX_CPU_H */
