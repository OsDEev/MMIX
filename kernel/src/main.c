/*
 * MyUnix kernel entry point.
 *
 * Boot protocol: Limine (base revision 3).
 */
#include <gdt.h>
#include <idt.h>
#include <io.h>
#include <kheap.h>
#include <lapic.h>
#include <libk.h>
#include <limine.h>
#include <pmap.h>
#include <pmm.h>
#include <sched.h>
#include <string.h>
#include <syscall.h>
#include <tty.h>
#include <chardev.h>
#include <vfs.h>
#include <mouse.h>

#include "elf.h"

/* === Limine requests === */

__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests")))
volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0
};

static void hcf(void) {
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

/* === Kernel entry === */

void _start(void) {
    serial_init();
    kprintf("[BOOT] MMix kernel starting...\n");

    /* Framebuffer / TTY */
    if (framebuffer_request.response != NULL &&
        framebuffer_request.response->framebuffer_count > 0) {
        tty_init(framebuffer_request.response->framebuffers[0]);
    } else {
        kprintf("[WARN] No framebuffer.\n");
    }

    /* PS/2 mouse (after TTY so we know screen dimensions) */
    mouse_init();

    /* HHDM & memory map */
    if (hhdm_request.response == NULL) hcf();
    if (memmap_request.response == NULL) hcf();
    kprintf("[BOOT] HHDM offset: 0x%x\n", hhdm_request.response->offset);

    uint64_t total_usable = 0;
    for (size_t i = 0; i < memmap_request.response->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap_request.response->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            total_usable += entry->length;
        }
    }
    kprintf("[BOOT] Usable RAM: %u MiB\n", total_usable / (1024 * 1024));

    /* PMM */
    pmm_init();

    /* GDT & IDT */
    gdt_init();
    idt_init();

    /* Syscalls + per-CPU data */
    syscall_init();

    /*
     * Address space: build the master kernel PML4 and switch to it.
     * Everything mapped into it from here on (LAPIC MMIO) is inherited
     * by every subsequently created address space.
     */
    kernel_pml4 = pmap_create();
    if (kernel_pml4 == 0) hcf();
    pmap_load(kernel_pml4);
    kprintf("[BOOT] Kernel page tables ready.\n");

    /* LAPIC timer replaces the PIT for scheduling.
     * The PIT channel 0 (~18.2 Hz) is programmed first: it drives the
     * reference ticks used to calibrate the LAPIC counter, then gets
     * masked by lapic_init(). */
    outb(0x43, 0x36);
    outb(0x40, 0x00);
    outb(0x40, 0x00);
    lapic_init();
    /* CoW frame refcounts */
    frame_refs_init(pmm_get_total_pages());
    /* Heap */
    heap_init();

    /* VFS, devfs, procfs & initrd */
    vfs_init();
    devfs_init();
    vfs_mount_procfs();
    bool have_initrd = false;
    if (module_request.response != NULL && module_request.response->module_count > 0) {
        struct limine_file *initrd = module_request.response->modules[0];
        vfs_load_initrd(initrd->address, initrd->size);
        have_initrd = true;
    } else {
        kprintf("[WARN] No initrd module loaded\n");
    }

    /* Scheduler */
    sched_init();

    /* Spawn /bin/init (PID 1, root) and /bin/rudod (PID 2, root daemon).
     * The shell drops itself to uid 1000 at startup. */
    if (have_initrd) {
        char *argv[] = {(char *)"/bin/init", NULL};
        struct exec_image img;
        if (exec_build_image("/bin/init", argv, &img) == 0) {
            int pid = sched_spawn_user_task("/bin/init", img.pml4_phys,
                                            img.entry, img.user_rsp, 0);
            if (pid >= 0) {
                task_t *init_task = sched_find_pid(pid);
                if (init_task != NULL) {
                    init_task->uid = 0;
                    init_task->brk_base = img.brk_base + PAGE_SIZE;
                    init_task->brk_cur = init_task->brk_base;
                }
            }
            kprintf("[BOOT] /bin/init scheduled as PID %d (root)\n", pid);
        } else {
            kprintf("[BOOT] PANIC: cannot load /bin/init\n");
            hcf();
        }

        char *rd_argv[] = {(char *)"/bin/rudod", NULL};
        struct exec_image rd_img;
        if (exec_build_image("/bin/rudod", rd_argv, &rd_img) == 0) {
            int rpid = sched_spawn_user_task("/bin/rudod", rd_img.pml4_phys,
                                             rd_img.entry, rd_img.user_rsp, 1);
            if (rpid >= 0) {
                task_t *rd = sched_find_pid(rpid);
                if (rd != NULL) {
                    rd->uid = 0;
                    rd->brk_base = rd_img.brk_base + PAGE_SIZE;
                    rd->brk_cur = rd->brk_base;
                }
            }
            kprintf("[BOOT] /bin/rudod scheduled as PID %d (root daemon)\n", rpid);
        }
    } else {
        hcf();
    }

    kprintf("[BOOT] Starting scheduler...\n");
    sched_start();
}
