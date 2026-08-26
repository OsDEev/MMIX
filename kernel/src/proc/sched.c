#include <cpu.h>
#include <gdt.h>
#include <kheap.h>
#include <libk.h>
#include <msr.h>
#include <pmap.h>
#include <pmm.h>
#include <sched.h>
#include <string.h>
#include <vfs.h>

/* Defined in syscall.c: shared stdin/stdout/stderr descriptions. */
extern file_t *console_fds[3];

int init_pid = -1; /* PID of the first user task (reparenting target) */

struct cpu_data bsp_cpu;

static task_t tasks[MAX_TASKS];
static task_t *current = NULL;
static bool scheduling_started = false;
static int next_pid = 1;

extern void context_switch(uint64_t *old_rsp, uint64_t new_rsp);
extern void jump_to_user(uint64_t entry, uint64_t user_stack);

task_t *sched_get_current(void) {
    return current;
}

void sched_wake(task_t *t) {
    if (t->state == TASK_BLOCKED && !t->zombie) {
        t->state = TASK_READY;
    }
}

/* --- context trampolines ------------------------------------------------ */

/*
 * First transition to ring 3 for a freshly spawned user task.
 * Reads the entry/stack stored in the task struct. Never returns.
 */
static void enter_first_userspace(void) {
    task_t *cur = current;

    /*
     * Fresh register file for the new user task. For fork()ed children
     * this is what makes RAX==0 (the fork contract); exec'd binaries
     * read argc/argv from the stack, not registers.
     */
    __asm__ volatile(
        "xor %%eax, %%eax \n"
        "xor %%ecx, %%ecx \n"
        "xor %%edx, %%edx \n"
        "xor %%esi, %%esi \n"
        "xor %%edi, %%edi \n"
        "xor %%r8d, %%r8d \n"
        "xor %%r9d, %%r9d \n"
        "xor %%r10d, %%r10d \n"
        "xor %%r11d, %%r11d \n"
        :
        :
        : "rax", "rcx", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11", "memory");

    jump_to_user(cur->user_entry, cur->user_rsp);

    for (;;) __asm__ volatile("hlt");
}

static void idle_task(void) {
    for (;;) {
        /*
         * Act on reschedule requests while idle: a task blocked in a
         * hlt-loop (keyboard read) can only be resumed by someone who
         * actually switches to it.
         */
        if (g_resched_requested) {
            g_resched_requested = false;
            sched_yield();
        }
        __asm__ volatile("sti");
        __asm__ volatile("hlt");
    }
}

/* --- task table --------------------------------------------------------- */

static task_t *alloc_slot(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_FREE) return &tasks[i];
    }
    return NULL;
}

static bool task_prepare_stack(task_t *t, void (*entry)(void)) {
    t->kstack = pmm_alloc(KSTACK_PAGES);
    if (t->kstack == NULL) return false;

    uint64_t *top = (uint64_t *)((uint8_t *)t->kstack + KSTACK_PAGES * PAGE_SIZE);

    top[-1] = (uint64_t)entry; /* ret pops this as RIP */
    top[-2] = 0;               /* r15 */
    top[-3] = 0;               /* r14 */
    top[-4] = 0;               /* r13 */
    top[-5] = 0;               /* r12 */
    top[-6] = 0;               /* rbx */
    top[-7] = 0;               /* rbp */

    t->rsp = (uint64_t)&top[-7];
    return true;
}

void sched_init(void) {
    memset(tasks, 0, sizeof(tasks));
    next_pid = 1;

    /* Idle task (PID 0) */
    task_t *idle = alloc_slot();
    if (idle == NULL || !task_prepare_stack(idle, idle_task)) {
        kprintf("[SCHED] PANIC: cannot create idle task\n");
        return;
    }
    idle->pid = 0;
    idle->parent_pid = 0;
    idle->state = TASK_READY;
    strncpy(idle->name, "idle", TASK_NAME_LEN - 1);

    kprintf("[SCHED] Initialized, idle task PID=0\n");
}

int sched_create_kernel_task(void (*entry)(void), const char *name) {
    task_t *t = alloc_slot();
    if (t == NULL) {
        kprintf("[SCHED] PANIC: max tasks reached\n");
        return -1;
    }

    memset(t, 0, sizeof(*t));
    t->pid = next_pid++;
    t->parent_pid = 0;
    t->state = TASK_READY;

    strncpy(t->name, name, TASK_NAME_LEN - 1);

    if (!task_prepare_stack(t, entry)) {
        kprintf("[SCHED] PANIC: cannot allocate kstack for '%s'\n", name);
        t->state = TASK_FREE;
        return -1;
    }

    kprintf("[SCHED] Created kernel task '%s' PID=%d\n", name, t->pid);
    return t->pid;
}

int sched_spawn_user_task(const char *name, uint64_t pml4_phys,
                          uint64_t entry, uint64_t user_rsp,
                          int parent_pid) {
    task_t *t = alloc_slot();
    if (t == NULL) return -1;

    memset(t, 0, sizeof(*t));
    t->pid = next_pid++;
    t->parent_pid = parent_pid;
    t->pgid = parent_pid; /* caller adjusts via setpgid/tcsetpgrp */
    t->sid = parent_pid;
    t->uid = 1000;        /* unprivileged by default; rudod gets 0 */
    t->state = TASK_READY;
    t->pml4_phys = pml4_phys;
    t->user_entry = entry;
    t->user_rsp = user_rsp;

    strncpy(t->name, name, TASK_NAME_LEN - 1);

    if (!task_prepare_stack(t, enter_first_userspace)) {
        t->state = TASK_FREE;
        return -1;
    }

    /* Default streams: stdin/stdout/stderr */
    for (int i = 0; i < 3 && i < MAX_FDS; i++) {
        if (console_fds[i] != NULL) t->fds[i] = *console_fds[i];
    }
    t->mmap_top = 0x50000000ULL;

    if (init_pid < 0) init_pid = t->pid;

    kprintf("[SCHED] Spawned '%s' PID=%d pml4=0x%x\n",
            name, t->pid, pml4_phys);
    return t->pid;
}

task_t *sched_find_pid(int pid) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_FREE && tasks[i].pid == pid) {
            return &tasks[i];
        }
    }
    return NULL;
}

task_t *sched_task_at(int index) {
    if (index < 0 || index >= MAX_TASKS) return NULL;
    return &tasks[index];
}

/* --- switching ---------------------------------------------------------- */

static void switch_to(task_t *next) {
    uint64_t ktop = (uint64_t)next->kstack + KSTACK_PAGES * PAGE_SIZE;

    tss_set_rsp0(ktop);
    bsp_cpu.kstack_top = ktop;
    pmap_load(next->pml4_phys ? next->pml4_phys : kernel_pml4);

    task_t *prev = current;

    /*
     * The saved user context lives in the (global) bsp_cpu slots while a
     * task runs; park it in the task struct across switches, otherwise a
     * task resuming from a syscall would read another task's context.
     */
    if (prev != NULL) {
        prev->uctx_rip = bsp_cpu.user_rip;
        prev->uctx_rsp = bsp_cpu.user_rsp;
    }
    bsp_cpu.user_rip = next->uctx_rip;
    bsp_cpu.user_rsp = next->uctx_rsp;

    current = next;
    context_switch(prev ? &prev->rsp : NULL, next->rsp);
}

void sched_yield(void) {
    if (!scheduling_started || current == NULL) return;

    task_t *prev = current;

    /* Round-robin over the whole table. */
    task_t *next = prev;
    int start = (int)(prev - tasks);
    for (int i = 1; i <= MAX_TASKS; i++) {
        task_t *cand = &tasks[(start + i) % MAX_TASKS];
        if (cand->state == TASK_READY || cand->state == TASK_RUNNING) {
            next = cand;
            break;
        }
    }

    if (next == prev) return;

    /*
     * Only a RUNNING task transitions out of the CPU here. Tasks that
     * parked themselves BLOCKED (pipe wait, waitpid) must keep that
     * state -- resetting it to READY would let them spin on the CPU
     * while logically still waiting.
     */
    if (prev->state == TASK_RUNNING) {
        prev->state = prev->zombie ? TASK_BLOCKED : TASK_READY;
    }
    next->state = TASK_RUNNING;

    switch_to(next);
}

volatile uint64_t g_uptime_ticks = 0;
volatile bool g_resched_requested = false;

void sched_timer_tick(void) {
    static uint64_t ticks = 0;
    ticks++;
    g_uptime_ticks++;

    /*
     * Never switch context from inside the ISR: just request a
     * reschedule. The actual switch happens at the next syscall
     * boundary (or kbd wait), on the task's own kernel stack.
     */
    if (scheduling_started && ticks % 5 == 0) {
        g_resched_requested = true;
    }
}

void sched_start(void) {
    scheduling_started = true;

    /* Jump straight into the highest-slot ready task (the init). */
    task_t *first = NULL;
    for (int i = MAX_TASKS - 1; i > 0; i--) {
        if (tasks[i].state == TASK_READY) {
            first = &tasks[i];
            break;
        }
    }
    if (first == NULL) {
        kprintf("[SCHED] Nothing to run!\n");
        return;
    }

    first->state = TASK_RUNNING;
    kprintf("[SCHED] Starting scheduler\n");

    /*
     * Critical section: an incoming timer tick here would nest a yield
     * on top of a half-finished switch. Interrupts are re-enabled
     * implicitly by the iret that drops into the first user task.
     */
    __asm__ volatile("cli");
    switch_to(first);

    kprintf("[BOOT] ERROR: scheduler returned!\n");
    for (;;) __asm__ volatile("cli; hlt");
}
