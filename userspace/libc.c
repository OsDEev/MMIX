#include "libc.h"

/* --- raw syscall wrappers ------------------------------------------------ */

/*
 * The kernel clobbers every caller-saved register (it is plain C over
 * there), so all of them must be declared clobbered/preserved-inout --
 * otherwise GCC may keep loop state in r8-r10 across a syscall.
 */
static inline long raw_syscall3(long n, long a, long b, long c) {
    register long r_num asm("rax") = n;
    register long r_a asm("rdi") = a;
    register long r_b asm("rsi") = b;
    register long r_c asm("rdx") = c;
    __asm__ volatile("syscall"
                     : "+r"(r_num), "+r"(r_a), "+r"(r_b), "+r"(r_c)
                     :
                     : "rcx", "r11", "r8", "r9", "r10", "memory");
    return r_num;
}

static inline long raw_syscall1(long n, long a) {
    return raw_syscall3(n, a, 0, 0);
}

/*
 * fork() keeps the callee-saved registers intact across the kernel call.
 * The child resumes at the instruction after `syscall` (the pops below),
 * with RAX=0 -- the fork contract.
 */
__asm__(
".global sys_fork          \n"
"sys_fork:                 \n"
"    push %rbx             \n"
"    push %rbp             \n"
"    push %r12             \n"
"    push %r13             \n"
"    push %r14             \n"
"    push %r15             \n"
"    mov $8, %eax          \n"
"    syscall               \n"
"    pop %r15              \n"
"    pop %r14              \n"
"    pop %r13              \n"
"    pop %r12              \n"
"    pop %rbp              \n"
"    pop %rbx              \n"
"    ret                   \n");

/* --- processes ----------------------------------------------------------- */

void sys_exit(int status) {
    raw_syscall1(SYS_EXIT, status);
    for (;;);
}

long sys_fork(void);
int execve(const char *path, char *const argv[]) {
    return (int)raw_syscall3(SYS_EXECVE, (long)path, (long)argv, 0);
}

int waitpid(int pid, int *status) {
    return (int)raw_syscall3(SYS_WAITPID, pid, (long)status, 0);
}

int getppid(void) { return (int)raw_syscall1(SYS_GETPPID, 0); }

int kill(int pid, int sig) {
    return (int)raw_syscall3(SYS_KILL, pid, sig, 0);
}

long signal(int sig, long handler) {
    return raw_syscall3(SYS_SIGNAL, sig, handler, 0);
}

int setpgid(int pid, int pgid) {
    return (int)raw_syscall3(SYS_SETPGID, pid, pgid, 0);
}

int getpgid(int pid) {
    return (int)raw_syscall1(SYS_GETPGID, pid);
}

int setsid(void) {
    return (int)raw_syscall1(SYS_SETSID, 0);
}

int tcsetpgrp(int pgid) {
    return (int)raw_syscall1(SYS_TCSETPGRP, pgid);
}

/* --- files, pipes, console ----------------------------------------------- */

long read(int fd, void *buf, size_t len) {
    return raw_syscall3(SYS_READ, fd, (long)buf, (long)len);
}

long write(int fd, const void *buf, size_t len) {
    return raw_syscall3(SYS_WRITE, fd, (long)buf, (long)len);
}

int open(const char *path) {
    return (int)raw_syscall3(SYS_OPEN, (long)path, 0, 0);
}

int open_append(const char *path) {
    return (int)raw_syscall3(SYS_OPEN, (long)path, O_APPEND, 0);
}

int open_create(const char *path) {
    return (int)raw_syscall3(SYS_OPEN, (long)path, O_CREATE, 0);
}

int close(int fd) { return (int)raw_syscall1(SYS_CLOSE, fd); }
int dup(int fd) { return (int)raw_syscall1(SYS_DUP, fd); }
int dup2(int oldfd, int newfd) {
    return (int)raw_syscall3(SYS_DUP2, oldfd, newfd, 0);
}

int pipe(int fds[2]) {
    return (int)raw_syscall1(SYS_PIPE, (long)fds);
}

int getpid(void) { return (int)raw_syscall1(SYS_GETPID, 0); }
void yield(void) { raw_syscall1(SYS_YIELD, 0); }
int brk_syscall(void *addr) { return (int)raw_syscall1(SYS_BRK, (long)addr); }
int uname(char *buf) { return (int)raw_syscall1(SYS_UNAME, (long)buf); }

int sysinfo(struct mmix_sysinfo *si) {
    return (int)raw_syscall1(SYS_SYSINFO, (long)si);
}

/* --- memory (malloc over anonymous mmap) --------------------------------- */

static char *mp_base = 0, *mp_ptr = 0, *mp_end = 0;

void *malloc(size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (n == 0) n = 16;

    if (mp_ptr + n > mp_end) {
        size_t need = (n + 4095) & ~(size_t)4095;
        long p = raw_syscall1(SYS_MMAP, (long)need);
        if (p < 0) return 0;
        mp_base = mp_ptr = (char *)p;
        mp_end = mp_base + need;
    }

    void *ret = mp_ptr;
    mp_ptr += n;
    return ret;
}

void *calloc(size_t n, size_t sz) {
    void *p = malloc(n * sz);
    if (p) memset(p, 0, n * sz);
    return p;
}

void free(void *p) { (void)p; /* bump allocator: no-op */ }

/* --- string/memory -------------------------------------------------------- */

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *(unsigned char *)a - *(unsigned char *)b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == '\0') {
            return *(unsigned char *)(a + i) - *(unsigned char *)(b + i);
        }
    }
    return 0;
}

void *memset(void *dst, int c, size_t n) {
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

char *strstr(const char *hay, const char *needle) {
    if (*needle == '\0') return (char *)hay;
    for (; *hay; hay++) {
        const char *h = hay, *nn = needle;
        while (*h && *nn && *h == *nn) { h++; nn++; }
        if (*nn == '\0') return (char *)hay;
    }
    return 0;
}

/* --- console helpers ------------------------------------------------------ */

void print(const char *s) {
    write(1, s, strlen(s));
}

void print_num(long n) {
    if (n < 0) {
        print("-");
        n = -n;
    }
    if (n >= 10) print_num(n / 10);
    char c = (char)('0' + n % 10);
    write(1, &c, 1);
}
