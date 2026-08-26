#ifndef MYUNIX_SYSCALL_H
#define MYUNIX_SYSCALL_H

#include <stdint.h>

/* Syscall numbers (keep in sync with userspace/libc.h) */
#define SYS_EXIT        0
#define SYS_WRITE       1
#define SYS_READ        2
#define SYS_OPEN        3
#define SYS_CLOSE       4
#define SYS_GETPID      5
#define SYS_YIELD       6
#define SYS_BRK         7
#define SYS_FORK        8
#define SYS_EXECVE      9
#define SYS_WAITPID     10
#define SYS_UNAME      11
#define SYS_SIGRETURN  12
#define SYS_GETPPID    13
#define SYS_MMAP       14
#define SYS_MUNMAP     15
#define SYS_DUP        16
#define SYS_DUP2       17
#define SYS_PIPE       18
#define SYS_KILL       19
#define SYS_SIGNAL     20
#define SYS_SYSINFO    21
#define SYS_SETPGID    22
#define SYS_GETPGID    23
#define SYS_SETSID     24
#define SYS_TCSETPGRP  25
#define SYS_TIME       26
#define SYS_REBOOT     27
#define SYS_GFX        28

#define MAX_SYSCALLS    64

/* Filled by SYS_SYSINFO (kernel -> user memory). */
struct mmix_timeval {
    uint32_t sec, min, hour;
    uint32_t day, mon, year;
    uint32_t uptime_s;
};

struct mmix_sysinfo {
    uint32_t total_ram_kb;
    uint32_t free_ram_kb;
    uint32_t uptime_s;
    uint32_t fb_w;
    uint32_t fb_h;
    uint32_t fb_bpp;
};

/* open() flags */
#define O_APPEND 0x1
#define O_CREATE 0x2

void syscall_init(void);
int64_t syscall_handler(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6);

#endif /* MYUNIX_SYSCALL_H */
