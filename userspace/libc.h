#ifndef MYUNIX_LIBC_H
#define MYUNIX_LIBC_H

#include <stddef.h>
#include <stdint.h>

/* Syscall numbers (keep in sync with kernel/src/sys/syscall.h) */
#define SYS_EXIT   0
#define SYS_WRITE  1
#define SYS_READ   2
#define SYS_OPEN   3
#define SYS_CLOSE  4
#define SYS_GETPID 5
#define SYS_YIELD  6
#define SYS_BRK    7
#define SYS_FORK   8
#define SYS_EXECVE 9
#define SYS_WAITPID 10
#define SYS_UNAME 11
#define SYS_SIGRETURN 12
#define SYS_GETPPID 13
#define SYS_MMAP 14
#define SYS_MUNMAP 15
#define SYS_DUP 16
#define SYS_DUP2 17
#define SYS_PIPE 18
#define SYS_KILL 19
#define SYS_SIGNAL 20
#define SYS_SYSINFO 21
#define SYS_TIME 26
#define SYS_REBOOT 27
#define SYS_GFX 28
#define SYS_PANIC 29
#define SYS_RUDO_REQUEST 30
#define SYS_RUDO_WAIT 31
#define SYS_SETUID 32
#define SYS_GETUID 33
#define SYS_SETPGID 22
#define SYS_GETPGID 23
#define SYS_SETSID 24
#define SYS_TCSETPGRP 25

/* Signals (subset) */
#define SIGINT  2
#define SIGSEGV 11
#define SIGPIPE 13
#define SIGTERM 15
#define SIGKILL 17
#define SIG_DFL 0
#define SIG_IGN 1

#define O_APPEND 0x1
#define O_CREATE 0x2

/* Processes */
void sys_exit(int status) __attribute__((noreturn));
long sys_fork(void);
int execve(const char *path, char *const argv[]);
int waitpid(int pid, int *status);
int getppid(void);
int kill(int pid, int sig);
int setpgid(int pid, int pgid);
int getpgid(int pid);
int setsid(void);
int tcsetpgrp(int pgid);
long signal(int sig, long handler);

/* Files, pipes, console */
long read(int fd, void *buf, size_t len);
long write(int fd, const void *buf, size_t len);
int open(const char *path);
int open_append(const char *path);
int open_create(const char *path);
int close(int fd);
int dup(int fd);
int dup2(int oldfd, int newfd);
int pipe(int fds[2]);
int getpid(void);
void yield(void);
int brk_syscall(void *addr);
int uname(char *buf);

/* System info */
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
int sysinfo(struct mmix_sysinfo *si);
int systime(struct mmix_timeval *tv);
void reboot(void) __attribute__((noreturn));
void kpanic(void) __attribute__((noreturn));
int rudo_request(int op);
int rudo_wait(int out[2]);
int setuid(int uid);
int getuid(void);
#define RUDO_OP_PANIC 1
#define RUDO_OP_SETUID 2
void gfx_fill_rect(int x, int y, int w, int h, uint32_t color);
void gfx_line(int x0, int y0, int x1, int y1, uint32_t color);
void gfx_circle(int cx, int cy, int r, uint32_t color);
void gfx_fill_circle(int cx, int cy, int r, uint32_t color);
void gfx_rect(int x, int y, int w, int h, uint32_t color);
void gfx_clear(uint32_t color);

/* Memory */
void *malloc(size_t n);
void *calloc(size_t n, size_t sz);
void free(void *p);

/* String/memory */
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
char *strstr(const char *hay, const char *needle);

/* Console helpers */
void print(const char *s);
void print_num(long n);

#endif /* MYUNIX_LIBC_H */
