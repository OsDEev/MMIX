#include <chardev.h>
#include <cpu.h>
#include <gdt.h>
#include <kbd.h>
#include <kheap.h>
#include <libk.h>
#include <limine.h>
#include <msr.h>
#include <pmap.h>
#include <pmm.h>
#include <lapic.h>
#include <pipe.h>
#include <rtc.h>
#include <gfx.h>
#include <sched.h>
#include <string.h>
#include <syscall.h>
#include <tty.h>
#include <vfs.h>

#include "../proc/elf.h"

extern volatile struct limine_hhdm_request hhdm_request;

#define HHDM_BASE (hhdm_request.response->offset)

/* MSR indices for the syscall/sysret fast path */
#define MSR_EFER        0xC0000080
#define MSR_STAR        0xC0000081
#define MSR_LSTAR       0xC0000082
#define MSR_FMASK       0xC0000084
#define MSR_GS_BASE     0xC0000101 /* current GS base */

#define EFER_SCE (1ULL << 0)

#define BRK_LIMIT 0x40000000ULL

/*
 * Foreground process group of /dev/console. Ctrl+C goes only here.
 */
long console_fg_pgid = 1;

/*
 * Shared console descriptions; every new task starts with copies of these
 * three as fds 0/1/2. Kept in syscall.c so sched.c can stay decoupled.
 */
static file_t console_fd_storage[3];
file_t *console_fds[3] = {
    &console_fd_storage[0], &console_fd_storage[1], &console_fd_storage[2]
};

typedef int64_t (*syscall_fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

/* ------------------------------------------------------------------ */
/* Process lifetime                                                    */
/* ------------------------------------------------------------------ */

static task_t *current_task(void) {
    return sched_get_current();
}

static void fds_release(task_t *t) {
    for (int fd = 0; fd < MAX_FDS; fd++) {
        if (t->fds[fd].pipe != NULL) {
            int rd = (t->fds[fd].flags & FDF_PIPE_RD) ? 1 : 0;
            int wr = (t->fds[fd].flags & FDF_PIPE_WR) ? 1 : 0;
            pipe_unref(t->fds[fd].pipe, rd, wr);
        }
        memset(&t->fds[fd], 0, sizeof(file_t));
    }
}

static void reap_task(task_t *t) {
    if (t->kstack != NULL) {
        pmm_free(t->kstack, KSTACK_PAGES);
    }
    if (t->pml4_phys != 0 && t->pml4_phys != kernel_pml4) {
        pmap_destroy(t->pml4_phys);
    }
    fds_release(t);
    memset(t, 0, sizeof(*t)); /* state becomes TASK_FREE */
}

/*
 * Terminate the current task because of a fatal signal (POSIX convention:
 * status = -signal). Never returns.
 */
static void terminate_current(int sig) {
    task_t *cur = current_task();
    cur->exit_status = -sig;
    cur->zombie = true;

    kprintf("[SIG] Task '%s' (PID %d) terminated by signal %d\n",
            cur->name, cur->pid, sig);

    task_t *parent = sched_find_pid(cur->parent_pid);
    if (parent != NULL) sched_wake(parent);

    fds_release(cur);
    sched_yield();

    for (;;) __asm__ volatile("cli; hlt");
}

/*
 * Deliver pending signals for the current task. Called at syscall entry.
 * May never return (fatal default action). Handlers are invoked with no
 * arguments; the trampoline at the signal frame calls SYS_SIGRETURN.
 */
#define SIGRET_CODE_SIZE 7
static const uint8_t sigret_code[SIGRET_CODE_SIZE] = {
    0xB8, 0x0C, 0x00, 0x00, 0x00, /* mov eax, SYS_SIGRETURN */
    0x0F, 0x05                    /* syscall */
};

static void deliver_signals(task_t *cur) {
    while (cur->sig_pending != 0) {
        int sig = __builtin_ctz(cur->sig_pending);
        uint64_t handler = cur->sig_handler[sig];

        if (handler == SIG_IGN) {
            cur->sig_pending &= ~(1u << sig);
            continue;
        }

        if (handler == SIG_DFL) {
            if (sig == SIGKILL || sig == SIGTERM || sig == SIGINT ||
                sig == SIGSEGV || sig == SIGPIPE) {
                terminate_current(sig);
            }
            cur->sig_pending &= ~(1u << sig); /* ignore others by default */
            continue;
        }

        /* User handler: build a signal frame on the user stack. */
        uint64_t saved_rip = bsp_cpu.user_rip;
        uint64_t saved_rsp = bsp_cpu.user_rsp;

        uint64_t frame = (saved_rsp - 64) & ~0xFULL;

        *(uint64_t *)(frame + 0)  = saved_rip;
        *(uint64_t *)(frame + 8)  = saved_rsp;
        memcpy((void *)(frame + 16), sigret_code, SIGRET_CODE_SIZE);

        cur->sig_pending &= ~(1u << sig);
        bsp_cpu.user_rip = handler;
        bsp_cpu.user_rsp = frame + 16;
        return; /* the syscall epilogue now enters the handler */
    }
}

static int64_t sys_exit(uint64_t status, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    task_t *cur = current_task();
    if (cur == NULL) {
        for (;;) __asm__ volatile("hlt");
    }

    kprintf("[SYSCALL] Task '%s' (PID %d) exited with status %d\n",
            cur->name, cur->pid, (int)status);

    cur->exit_status = (int)status;
    cur->zombie = true;

    /* Wake a blocked parent so it can reap us. */
    task_t *parent = sched_find_pid(cur->parent_pid);
    if (parent != NULL) sched_wake(parent);

    sched_yield();

    /* Should never get here: zombies are not scheduled. */
    for (;;) __asm__ volatile("hlt");
    return 0;
}

static int64_t sys_fork(uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    task_t *cur = current_task();
    if (cur == NULL || cur->user_entry == 0) return -1;

    /* User context captured by the syscall entry stub. */
    uint64_t user_rip = bsp_cpu.user_rip;
    uint64_t user_rsp = bsp_cpu.user_rsp;

    uint64_t child_pml4 = pmap_fork(cur->pml4_phys);
    if (child_pml4 == 0) {
        kprintf("[FORK] OOM: cannot duplicate address space\n");
        return -1;
    }

    /*
     * Flush THIS cpu's TLB: the parent's stale writable translations
     * must not survive now that its pages are marked read-only (CoW),
     * otherwise it would silently scribble over the shared snapshot.
     */
    pmap_load(cur->pml4_phys);

    char name[TASK_NAME_LEN];
    strncpy(name, cur->name, TASK_NAME_LEN - 1);

    int pid = sched_spawn_user_task(name, child_pml4, user_rip, user_rsp,
                                    cur->pid);
    if (pid < 0) {
        pmap_destroy(child_pml4);
        return -1;
    }

    /* Clone the full fd table and heap position. */
    task_t *child = sched_find_pid(pid);
    memcpy(child->fds, cur->fds, sizeof(cur->fds));
    child->brk_base = cur->brk_base;
    child->brk_cur = cur->brk_cur;
    child->mmap_top = cur->mmap_top;

    /* Duplicate pipe references for the child's copied fd table. */
    for (int fd = 0; fd < MAX_FDS; fd++) {
        if (child->fds[fd].pipe != NULL) {
            int rd = (child->fds[fd].flags & FDF_PIPE_RD) ? 1 : 0;
            int wr = (child->fds[fd].flags & FDF_PIPE_WR) ? 1 : 0;
            pipe_ref(child->fds[fd].pipe, rd, wr);
        }
    }

    /*
     * The child resumes at the same user RIP (right after the `syscall`
     * instruction) with all GP registers zeroed except RAX=0 -- which is
     * exactly the fork() contract. Userspace wrappers preserve their own
     * callee-saved registers across the call.
     */
    return pid;
}

static int64_t sys_execve(uint64_t path_ptr, uint64_t argv_ptr, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    task_t *cur = current_task();

    const char *path = (const char *)path_ptr;
    char **user_argv = (char **)argv_ptr;

    if (path == NULL) return -1;

    /* Copy argv from user memory into kernel buffers. */
    char *argv_copy[EXEC_MAX_ARGS + 1];
    static char arg_strings[EXEC_MAX_ARGS][EXEC_MAX_ARG_LEN];

    int argc = 0;
    if (user_argv != NULL) {
        while (argc < EXEC_MAX_ARGS && user_argv[argc] != NULL) {
            strncpy(arg_strings[argc], user_argv[argc], EXEC_MAX_ARG_LEN - 1);
            arg_strings[argc][EXEC_MAX_ARG_LEN - 1] = '\0';
            argv_copy[argc] = arg_strings[argc];
            argc++;
        }
    }
    argv_copy[argc] = NULL;

    struct exec_image img;
    if (exec_build_image(path, argv_copy, &img) != 0) return -1;

    uint64_t old_pml4 = cur->pml4_phys;


    cur->pml4_phys = img.pml4_phys;
    cur->brk_base = img.brk_base + PAGE_SIZE;
    cur->brk_cur = cur->brk_base;
    cur->mmap_top = 0x50000000ULL;
    strncpy(cur->name, path, TASK_NAME_LEN - 1);

    /* Jump straight into the new image: patch the saved user context and
     * replicate the syscall epilogue. Never returns. */
    pmap_load(img.pml4_phys);
    pmap_destroy(old_pml4);

    bsp_cpu.user_rsp = img.user_rsp;
    bsp_cpu.user_rip = img.entry;


    __asm__ volatile(
        "movabs $bsp_cpu, %%rsi \n"
        "mov 8(%%rsi), %%rsp    \n"   /* CPU_USER_RSP */
        "mov 16(%%rsi), %%rcx   \n"   /* CPU_USER_RIP */
        ".byte 0x48; sysret     \n"
        :
        :
        : "memory");

    for (;;);
}

static int64_t sys_waitpid(uint64_t pid, uint64_t status_ptr, uint64_t flags,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)flags; (void)a4; (void)a5; (void)a6;
    task_t *cur = current_task();

    for (;;) {
        bool have_children = false;

        for (int i = 0; i < MAX_TASKS; i++) {
            task_t *t = sched_task_at(i);
            if (t == NULL || t->state == TASK_FREE) continue;
            if (t->parent_pid != (int)cur->pid) continue;
            if (t->pid == 0) continue; /* idle */

            have_children = true;

            bool want = ((int64_t)pid == -1) || ((int)pid == t->pid);
            if (!want) continue;

            if (t->zombie) {
                if (status_ptr != 0) {
                    *(int *)status_ptr = t->exit_status;
                }
                int reaped_pid = t->pid;
                reap_task(t);
                return reaped_pid;
            }
        }

        if (!have_children) return -1;

        cur->state = TASK_BLOCKED;
        sched_yield();
    }
}

/* ------------------------------------------------------------------ */
/* File descriptors                                                    */
/* ------------------------------------------------------------------ */

static file_t *fd_get(task_t *t, int fd) {
    if (fd < 0 || fd >= MAX_FDS) return NULL;
    file_t *f = &t->fds[fd];
    if (f->node == NULL && f->pipe == NULL && f->flags == 0) return NULL;
    return f;
}

static int fd_alloc(task_t *t) {
    for (int fd = 0; fd < MAX_FDS; fd++) {
        if (t->fds[fd].node == NULL && t->fds[fd].pipe == NULL &&
            t->fds[fd].flags == 0) {
            return fd;
        }
    }
    return -1;
}

/* Drop one reference from an fd slot (pipe-aware). */
static void fd_put(task_t *t, int fd) {
    file_t *f = &t->fds[fd];
    if (f->pipe != NULL) {
        int rd = (f->flags & FDF_PIPE_RD) ? 1 : 0;
        int wr = (f->flags & FDF_PIPE_WR) ? 1 : 0;
        pipe_unref(f->pipe, rd, wr);
    }
    memset(f, 0, sizeof(file_t));
}

static int64_t sys_open(uint64_t path_ptr, uint64_t flags, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    task_t *cur = current_task();
    const char *path = (const char *)path_ptr;
    if (path == NULL || cur == NULL) return -1;

    vfs_node_t *node = vfs_resolve(path);
    if ((node == NULL || node->type != VFS_FILE) && (flags & O_CREATE)) {
        node = vfs_create_file(path);
    }
    if (node == NULL) return -1;
    if (node->type != VFS_FILE && node->type != VFS_CHARDEV &&
        node->type != VFS_PROCFS) {
        return -1;
    }

    int fd = fd_alloc(cur);
    if (fd < 0) return -1;

    memset(&cur->fds[fd], 0, sizeof(file_t));
    cur->fds[fd].node = node;
    if (flags & O_APPEND) {
        cur->fds[fd].pos = node->size;
    }
    return fd;
}

static int64_t sys_close(uint64_t fd, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    task_t *cur = current_task();
    if (!fd_get(cur, (int)fd)) return -1;

    fd_put(cur, (int)fd);
    return 0;
}

static int64_t sys_read(uint64_t fd, uint64_t buf_ptr, uint64_t count,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    task_t *cur = current_task();
    char *buf = (char *)buf_ptr;

    file_t *f = fd_get(cur, (int)fd);
    if (f == NULL || buf == NULL || count == 0) return -1;

    /* Character device */
    if (f->node != NULL && f->node->type == VFS_CHARDEV) {
        struct chardev *cd = (struct chardev *)f->node->dev;
        return cd ? cd->read(buf, count) : -1;
    }

    /* procfs: dynamic content through the VFS layer */
    if (f->node != NULL && f->node->type == VFS_PROCFS) {
        int r = vfs_read(f->node, buf, count, f->pos);
        if (r > 0) f->pos += (size_t)r;
        return r;
    }

    /* Pipe read end */
    if (f->flags & FDF_PIPE_RD) {
        return pipe_read(f->pipe, buf, count);
    }

    /* Keyboard */
    if (f->flags & FDF_CONSOLE_IN) {
        size_t n = 0;
        while (n < count) {
            int c = kbd_getchar();
            if (c < 0) {
                if (n == 0) return -1; /* interrupted by a signal */
                break;
            }
            tty_putc((char)c); /* echo */
            buf[n++] = (char)c;
            if (c == '\n') break;
        }
        return (int64_t)n;
    }

    if (f->node == NULL) return -1;
    int r = vfs_read(f->node, buf, count, f->pos);
    if (r > 0) f->pos += (size_t)r;
    return r;
}

static int64_t sys_write(uint64_t fd, uint64_t buf_ptr, uint64_t count,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    task_t *cur = current_task();
    const char *buf = (const char *)buf_ptr;

    file_t *f = fd_get(cur, (int)fd);
    if (f == NULL || buf == NULL || count == 0) return -1;

    /* Character device */
    if (f->node != NULL && f->node->type == VFS_CHARDEV) {
        struct chardev *cd = (struct chardev *)f->node->dev;
        return cd ? cd->write(buf, count) : -1;
    }

    if (f->flags & FDF_PIPE_WR) {
        return pipe_write(f->pipe, buf, count);
    }

    if (f->flags & FDF_CONSOLE_OUT) {
        tty_write(buf, count);
        return (int64_t)count;
    }

    if (f->node == NULL) return -1;
    int r = vfs_write(f->node, buf, count, f->pos);
    if (r > 0) f->pos += (size_t)r;
    return r;
}

/* ------------------------------------------------------------------ */
/* Misc                                                                */
/* ------------------------------------------------------------------ */

static int64_t sys_getpid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                          uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    task_t *cur = current_task();
    if (cur == NULL) return -1;
    return cur->pid;
}

static int64_t sys_yield(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                         uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    sched_yield();
    return 0;
}

/* ------------------------------------------------------------------ */
/* IPC: pipes, dup, signals                                            */
/* ------------------------------------------------------------------ */

static int64_t sys_pipe(uint64_t ufds, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    task_t *cur = current_task();
    int *user_fds = (int *)ufds;

    struct pipe *p = pipe_create();
    if (p == NULL) return -1;

    int rd = fd_alloc(cur);
    if (rd < 0) { pipe_unref(p, 1, 1); return -1; }

    /* Mark the slot busy before allocating the second one. */
    memset(&cur->fds[rd], 0, sizeof(file_t));
    cur->fds[rd].pipe = p;

    int wr = fd_alloc(cur);
    if (wr < 0) {
        memset(&cur->fds[rd], 0, sizeof(file_t));
        pipe_unref(p, 1, 1);
        return -1;
    }

    memset(&cur->fds[wr], 0, sizeof(file_t));
    cur->fds[rd].flags = FDF_PIPE_RD;
    cur->fds[wr].pipe = p;
    cur->fds[wr].flags = FDF_PIPE_WR;

    user_fds[0] = rd;
    user_fds[1] = wr;
    return 0;
}

static int64_t do_dup2(task_t *cur, int oldfd, int newfd) {
    if (!fd_get(cur, oldfd)) return -1;
    if (newfd < 0 || newfd >= MAX_FDS) return -1;
    if (oldfd == newfd) return newfd;

    if (fd_get(cur, newfd)) fd_put(cur, newfd);

    cur->fds[newfd] = cur->fds[oldfd];
    if (cur->fds[newfd].pipe != NULL) {
        int rd = (cur->fds[newfd].flags & FDF_PIPE_RD) ? 1 : 0;
        int wr = (cur->fds[newfd].flags & FDF_PIPE_WR) ? 1 : 0;
        pipe_ref(cur->fds[newfd].pipe, rd, wr);
    }
    return newfd;
}

static int64_t sys_dup(uint64_t fd, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    task_t *cur = current_task();
    if (!fd_get(cur, (int)fd)) return -1;

    int newfd = fd_alloc(cur);
    if (newfd < 0) return -1;
    return do_dup2(cur, (int)fd, newfd);
}

static int64_t sys_dup2(uint64_t oldfd, uint64_t newfd, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    task_t *cur = current_task();
    return do_dup2(cur, (int)oldfd, (int)newfd);
}

static int64_t sys_getppid(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    task_t *cur = current_task();
    if (cur == NULL) return -1;
    return cur->parent_pid;
}

static int64_t sys_kill(uint64_t pid, uint64_t sig, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (sig == 0 || sig >= NSIG) return -1;

    task_t *target = sched_find_pid((int)pid);
    if (target == NULL || target->pid == 0) return -1;

    if (target == current_task()) {
        target->sig_pending |= (1u << sig);
        deliver_signals(target); /* may not return */
        return 0;
    }

    target->sig_pending |= (1u << sig);

    /* SIGKILL on a parked task: finish it right here. */
    if (sig == SIGKILL && (target->state == TASK_BLOCKED ||
                           target->state == TASK_READY)) {
        target->exit_status = -SIGKILL;
        target->zombie = true;
        kprintf("[SIG] Task '%s' (PID %d) killed\n", target->name, target->pid);
        task_t *parent = sched_find_pid(target->parent_pid);
        if (parent != NULL) sched_wake(parent);
        if (target->wait_pipe != NULL) {
            struct pipe *wp = target->wait_pipe;
            target->wait_pipe = NULL;
            /* Wake so the parked syscall unwinds; it will see the zombie. */
            for (int i = 0; i < MAX_TASKS; i++) {
                task_t *t = sched_task_at(i);
                if (t != NULL && t->state == TASK_BLOCKED && t->wait_pipe == wp) {
                    t->wait_pipe = NULL;
                    sched_wake(t);
                }
            }
        } else {
            sched_wake(target); /* no-op for READY, needed for BLOCKED */
        }
    } else if (target->wait_pipe != NULL) {
        /* Wake pipe-blocked tasks so they can run their abort checks. */
        struct pipe *wp = target->wait_pipe;
        for (int i = 0; i < MAX_TASKS; i++) {
            task_t *t = sched_task_at(i);
            if (t != NULL && t->state == TASK_BLOCKED && t->wait_pipe == wp) {
                t->wait_pipe = NULL;
                sched_wake(t);
            }
        }
    }

    return 0;
}

static int64_t sys_signal(uint64_t sig, uint64_t handler, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    task_t *cur = current_task();
    if (sig == 0 || sig >= NSIG || sig == SIGKILL) return -1;

    int64_t old = (int64_t)cur->sig_handler[sig];
    cur->sig_handler[sig] = handler;
    return old;
}

static int64_t sys_sigreturn(uint64_t a1, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    task_t *cur = current_task();

    /* The trampoline pushed a frame: [rsp-16]=saved rip, [rsp-8]=saved rsp */
    uint64_t frame_rsp = bsp_cpu.user_rsp;
    bsp_cpu.user_rip = *(uint64_t *)(frame_rsp - 16);
    bsp_cpu.user_rsp = *(uint64_t *)(frame_rsp - 8);
    (void)cur;
    return 0;
}

static int64_t sys_setpgid(uint64_t pid, uint64_t pgid, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    task_t *cur = current_task();
    task_t *t = ((int64_t)pid == 0) ? cur : sched_find_pid((int)pid);
    if (t == NULL || t->sid != cur->sid) return -1;
    if (t->zombie) return -1;

    int newpgid = ((int64_t)pgid == 0) ? t->pid : (int)pgid;
    /* Only creating a new group led by the task itself is supported. */
    if (newpgid != t->pid) return -1;
    t->pgid = newpgid;
    return 0;
}

static int64_t sys_getpgid(uint64_t pid, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    task_t *t = ((int64_t)pid == 0) ? current_task() : sched_find_pid((int)pid);
    return (t != NULL) ? t->pgid : -1;
}

static int64_t sys_setsid(uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    task_t *cur = current_task();
    if (cur == NULL) return -1;

    /* A session leader cannot create a new session. */
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = sched_task_at(i);
        if (t != NULL && t->state != TASK_FREE && t->sid == cur->pid &&
            t->pid != cur->pid) {
            return -1;
        }
    }

    cur->sid = cur->pid;
    cur->pgid = cur->pid;
    return cur->sid;
}

static int64_t sys_tcsetpgrp(uint64_t pgid, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    task_t *cur = current_task();
    if (cur == NULL) return -1;
    if ((int)pgid != cur->pgid && sched_find_pid((int)pgid) == NULL) return -1;
    console_fg_pgid = (long)pgid;
    return 0;
}

static int64_t sys_time(uint64_t out_ptr, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct mmix_timeval *tv = (struct mmix_timeval *)out_ptr;
    if (tv == NULL) return -1;

    struct mmix_rtc rtc;
    rtc_read(&rtc);
    tv->sec = rtc.sec; tv->min = rtc.min; tv->hour = rtc.hour;
    tv->day = rtc.day; tv->mon = rtc.mon; tv->year = rtc.year;
    tv->uptime_s = (uint32_t)(g_uptime_ticks / TIMER_HZ);
    return 0;
}

static int64_t sys_reboot(uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    kprintf("[MMIX] Rebooting...\n");

    /* 8042 keyboard controller reset pulse */
    for (volatile int i = 0; i < 100000; i++) { }
    __asm__ volatile("outb %%al, $0x64" :: "a"((uint8_t)0xFE));

    /* Fallback: triple fault via a far return to a null IDT entry */
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
    return 0;
}

/* GFX ops */
#define GFX_FILL_RECT 1
#define GFX_LINE      2
#define GFX_CIRCLE    3
#define GFX_CLEAR     4
#define GFX_FILL_CIRCLE 5
#define GFX_RECT      6

static int64_t sys_gfx(uint64_t op, uint64_t a, uint64_t b, uint64_t c,
                       uint64_t d, uint64_t color) {
    switch (op) {
        case GFX_FILL_RECT:
            gfx_fill_rect((int)a, (int)b, (int)c, (int)d, (uint32_t)color);
            return 0;
        case GFX_RECT:
            gfx_rect((int)a, (int)b, (int)c, (int)d, (uint32_t)color);
            return 0;
        case GFX_LINE:
            gfx_line((int)a, (int)b, (int)c, (int)d, (uint32_t)color);
            return 0;
        case GFX_CIRCLE:
            gfx_circle((int)a, (int)b, (int)c, (uint32_t)color);
            return 0;
        case GFX_FILL_CIRCLE:
            gfx_fill_circle((int)a, (int)b, (int)c, (uint32_t)color);
            return 0;
        case GFX_CLEAR:
            gfx_clear((uint32_t)a);
            return 0;
        default:
            return -1;
    }
}

static int64_t sys_sysinfo(uint64_t out_ptr, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct mmix_sysinfo *si = (struct mmix_sysinfo *)out_ptr;
    if (si == NULL) return -1;

    uint64_t total_kb = pmm_get_usable_bytes() / 1024;
    uint64_t free_kb = (pmm_get_free_pages() * PAGE_SIZE) / 1024;

    si->total_ram_kb = (uint32_t)total_kb;
    si->free_ram_kb  = (uint32_t)free_kb;
    si->uptime_s     = (uint32_t)(g_uptime_ticks / TIMER_HZ);
    tty_get_mode(&si->fb_w, &si->fb_h, &si->fb_bpp);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Virtual memory: anonymous mmap/munmap                               */
/* ------------------------------------------------------------------ */

#define MMAP_AREA_BASE 0x50000000ULL
#define MMAP_AREA_END  0x6FFFF000ULL

static int64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot,
                        uint64_t flags, uint64_t fd, uint64_t off) {
    (void)addr; (void)prot; (void)flags; (void)fd; (void)off;
    task_t *cur = current_task();
    if (cur == NULL || len == 0) return -1;

    len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (cur->mmap_top == 0) cur->mmap_top = MMAP_AREA_BASE;
    if (cur->mmap_top + len > MMAP_AREA_END) return -1;

    uint64_t base = cur->mmap_top;
    for (uint64_t va = base; va < base + len; va += PAGE_SIZE) {
        void *frame = pmm_alloc(1);
        if (frame == NULL) return -1;
        memset(frame, 0, PAGE_SIZE);
        pmap_map(cur->pml4_phys, va,
                 (uint64_t)frame - HHDM_BASE, PTE_USER | PTE_WRITE);
    }

    cur->mmap_top += len;
    return (int64_t)base;
}

static int64_t sys_munmap(uint64_t addr, uint64_t len, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    task_t *cur = current_task();
    if (cur == NULL || len == 0 || (addr & 0xFFFULL) != 0) return -1;

    len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uint64_t va = addr; va < addr + len; va += PAGE_SIZE) {
        uint64_t pa = pmap_resolve(cur->pml4_phys, va);
        if (pa != 0) {
            if (frame_ref_dec(pa)) {
                pmm_free((void *)(pa + HHDM_BASE), 1);
            }
            pmap_unmap(cur->pml4_phys, va);
        }
    }
    return 0;
}

static int64_t sys_uname(uint64_t buf_ptr, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    char *buf = (char *)buf_ptr;
    if (buf == NULL) return -1;
    const char *info = "MMix 0.3.0 x86_64";
    memcpy(buf, info, strlen(info) + 1);
    return 0;
}

static int64_t sys_brk(uint64_t addr, uint64_t a2, uint64_t a3, uint64_t a4,
                       uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    task_t *cur = current_task();
    if (cur == NULL) return -1;

    if (addr == 0) return (int64_t)cur->brk_cur;

    if (addr < cur->brk_base || addr >= BRK_LIMIT) {
        return (int64_t)cur->brk_cur;
    }

    uint64_t old_top = (cur->brk_cur + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t new_top = (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (new_top > old_top) {
        for (uint64_t va = old_top; va < new_top; va += PAGE_SIZE) {
            void *frame = pmm_alloc(1);
            if (frame == NULL) break;
            memset(frame, 0, PAGE_SIZE);
            pmap_map(cur->pml4_phys, va,
                     (uint64_t)frame - HHDM_BASE, PTE_USER | PTE_WRITE);
        }
    } else if (new_top < old_top) {
        for (uint64_t va = new_top; va < old_top; va += PAGE_SIZE) {
            uint64_t pa = pmap_resolve(cur->pml4_phys, va);
            if (pa != 0 && frame_ref_dec(pa)) {
                pmm_free((void *)(pa + HHDM_BASE), 1);
            }
        }
    }

    cur->brk_cur = addr;
    return (int64_t)addr;
}

/* === Syscall table === */

static syscall_fn syscall_table[MAX_SYSCALLS] = {
    [SYS_EXIT]    = sys_exit,
    [SYS_WRITE]   = sys_write,
    [SYS_READ]    = sys_read,
    [SYS_OPEN]    = sys_open,
    [SYS_CLOSE]   = sys_close,
    [SYS_GETPID]  = sys_getpid,
    [SYS_YIELD]   = sys_yield,
    [SYS_BRK]     = sys_brk,
    [SYS_FORK]    = sys_fork,
    [SYS_EXECVE]  = sys_execve,
    [SYS_WAITPID] = sys_waitpid,
    [SYS_UNAME]   = sys_uname,
    [SYS_SIGRETURN] = sys_sigreturn,
    [SYS_GETPPID] = sys_getppid,
    [SYS_MMAP]    = sys_mmap,
    [SYS_MUNMAP]  = sys_munmap,
    [SYS_DUP]     = sys_dup,
    [SYS_DUP2]    = sys_dup2,
    [SYS_PIPE]    = sys_pipe,
    [SYS_KILL]    = sys_kill,
    [SYS_SIGNAL]  = sys_signal,
    [SYS_SYSINFO] = sys_sysinfo,
    [SYS_TIME]    = sys_time,
    [SYS_REBOOT]  = sys_reboot,
    [SYS_GFX]     = sys_gfx,
    [SYS_SETPGID] = sys_setpgid,
    [SYS_GETPGID] = sys_getpgid,
    [SYS_SETSID]  = sys_setsid,
    [SYS_TCSETPGRP] = sys_tcsetpgrp,
};

int64_t syscall_handler(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    task_t *cur = current_task();
    if (cur != NULL && cur->sig_pending != 0) {
        deliver_signals(cur); /* may never return */
    }

    /* Preemption point: the timer only requests, we act here (IF=0). */
    if (g_resched_requested) {
        g_resched_requested = false;
        sched_yield();
    }

    if (num >= MAX_SYSCALLS || syscall_table[num] == NULL) {
        kprintf("[SYSCALL] Invalid syscall number: %u\n", num);
        return -1;
    }
    return syscall_table[num](a1, a2, a3, a4, a5, a6);
}

void syscall_init(void) {
    console_fd_storage[0].flags = FDF_CONSOLE_IN;
    console_fd_storage[1].flags = FDF_CONSOLE_OUT;
    console_fd_storage[2].flags = FDF_CONSOLE_OUT;

    /* Enable SCE (System Call Extensions) in IA32_EFER */
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_SCE;
    wrmsr(MSR_EFER, efer);

    /* IA32_STAR: kernel CS/SS in bits 47:32, user CS/SS in bits 63:48 */
    wrmsr(MSR_STAR,
          ((uint64_t)GDT_KERNEL_CODE << 32) | ((uint64_t)GDT_USER_CODE << 48));

    /* IA32_LSTAR: syscall entry point */
    extern void syscall_entry(void);
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* IA32_FMASK: mask IF during syscall */
    wrmsr(MSR_FMASK, 0x200);

    /*
     * Per-CPU data for the syscall path.
     *
     * Invariant: while executing in the kernel, GS_BASE points at
     * struct cpu_data; swapgs on the syscall boundary flips it with the
     * (zero) user base, so KGS_BASE carries the kernel pointer while in
     * ring 3.
     */
    wrmsr(MSR_GS_BASE, (uint64_t)&bsp_cpu);
    kprintf("[SYSCALL] Initialized (gs_base=%x, readback=%x)\n",
            (uint64_t)&bsp_cpu, rdmsr(MSR_GS_BASE));
}
