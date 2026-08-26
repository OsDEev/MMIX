#ifndef MYUNIX_SCHED_H
#define MYUNIX_SCHED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_TASKS 64
#define TASK_NAME_LEN 32
#define KSTACK_PAGES 32 /* 128 KiB kernel stack per task */
#define MAX_FDS 16
#define NSIG 32

/* Signal numbers (subset of POSIX) */
#define SIGINT  2
#define SIGSEGV 11
#define SIGPIPE 13
#define SIGTERM 15
#define SIGKILL 17

#define SIG_DFL 0 /* default action */
#define SIG_IGN 1 /* ignore */

typedef enum {
    TASK_FREE = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED /* waiting (keyboard, pipe, waitpid) or unreaped zombie */
} task_state_t;

/* Forward declarations; full types live in fs/vfs.h and fs/pipe.h. */
struct vfs_node;
struct pipe;

/*
 * Open file description. Stored BY VALUE in the fd table so that fork()
 * duplicates it naturally.
 */
typedef struct file {
    struct vfs_node *node; /* NULL => console or pipe */
    struct pipe *pipe;     /* NULL unless this fd is a pipe end */
    int flags;             /* FDF_* */
    size_t pos;
} file_t;

#define FDF_CONSOLE_IN  0x1 /* keyboard */
#define FDF_CONSOLE_OUT 0x2 /* tty */
#define FDF_PIPE_RD     0x4 /* read end of a pipe */
#define FDF_PIPE_WR     0x8 /* write end of a pipe */

typedef struct task {
    int pid;
    int parent_pid;
    int pgid;  /* process group id */
    int sid;   /* session id */
    task_state_t state;
    bool zombie;         /* exited, awaiting waitpid() */
    int exit_status;

    char name[TASK_NAME_LEN];

    /* Context */
    uint64_t rsp;        /* kernel rsp at last switch */
    uint64_t uctx_rip;   /* saved user context (mirrors bsp_cpu slots) */
    uint64_t uctx_rsp;

    /* Kernel stack */
    void *kstack;

    /* Address space */
    uint64_t pml4_phys;

    /* User entry (used only for the very first transition to ring 3) */
    uint64_t user_entry;
    uint64_t user_rsp;

    /* User heap (brk) */
    uintptr_t brk_base;
    uintptr_t brk_cur;

    /* mmap arena (bump allocator, grows up) */
    uintptr_t mmap_top;

    /* File descriptors */
    file_t fds[MAX_FDS];

    /* Signals */
    uint32_t sig_pending;
    uint64_t sig_handler[NSIG]; /* user fn ptr, SIG_DFL or SIG_IGN */

    /* Blocking channel: set while parked inside a pipe op */
    struct pipe *wait_pipe;
} task_t;

void sched_init(void);
int sched_create_kernel_task(void (*entry)(void), const char *name);
int sched_spawn_user_task(const char *name, uint64_t pml4_phys,
                          uint64_t entry, uint64_t user_rsp, int parent_pid);
void sched_yield(void);
void sched_start(void);
void sched_timer_tick(void);
task_t *sched_get_current(void);
task_t *sched_find_pid(int pid);
task_t *sched_task_at(int index); /* NULL if out of range */
void sched_wake(task_t *t);

extern int init_pid;
extern volatile uint64_t g_uptime_ticks;
extern volatile bool g_resched_requested;

#endif /* MYUNIX_SCHED_H */
