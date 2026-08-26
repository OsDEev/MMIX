#include <gdt.h>
#include <idt.h>
#include <io.h>
#include <kbd.h>
#include <lapic.h>
#include <libk.h>
#include <mouse.h>
#include <msr.h>
#include <panic.h>
#include <pmap.h>
#include <sched.h>
#include <string.h>

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* PIC ports/commands */
#define PIC1_CMD 0x20
#define PIC1_DAT 0x21
#define PIC2_CMD 0xA0
#define PIC2_DAT 0xA1
#define PIC_EOI  0x20

#define IDT_KERNEL_GATE 0x8E /* present, ring 0, interrupt gate */

static struct idt_entry idt[256];
static struct idt_ptr idtr;

static const char *exception_messages[] = {
    "Division By Zero",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack-Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 Floating-Point Exception",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor Injection Exception",
    "VMM Communication Exception",
    "Security Exception",
    "Reserved"
};

/* Stubs defined in isr.asm */
static void (*const exception_stubs[32])(void) = {
    isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
    isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
    isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
};

static void (*const irq_stubs[IRQ_COUNT])(void) = {
    irq0,  irq1,  irq2,  irq3,  irq4,  irq5,  irq6,  irq7,
    irq8,  irq9,  irq10, irq11, irq12, irq13, irq14, irq15
};

void idt_set_gate(uint8_t vector, uint64_t handler, uint16_t selector, uint8_t flags) {
    idt[vector].offset_low  = handler & 0xFFFF;
    idt[vector].selector    = selector;
    idt[vector].ist         = 0;
    idt[vector].type_attr   = flags;
    idt[vector].offset_mid  = (handler >> 16) & 0xFFFF;
    idt[vector].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[vector].zero        = 0;
}

static void pic_remap(uint8_t offset1, uint8_t offset2) {
    outb(PIC1_CMD, 0x11);
    outb(PIC2_CMD, 0x11);
    outb(PIC1_DAT, offset1);
    outb(PIC2_DAT, offset2);
    outb(PIC1_DAT, 0x04); /* Slave on IRQ2 */
    outb(PIC2_DAT, 0x02);
    outb(PIC1_DAT, 0x01);
    outb(PIC2_DAT, 0x01);

    /* Unmask all IRQs */
    outb(PIC1_DAT, 0x00);
    outb(PIC2_DAT, 0x00);
}

static inline void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_CMD, PIC_EOI);
    }
    outb(PIC1_CMD, PIC_EOI);
}

/* PIT ticks, used by the LAPIC calibration and exported for it. */
volatile uint64_t g_pit_ticks = 0;

void isr_handler(uint64_t int_no, uint64_t err_code, struct interrupt_frame *frame) {
    if (int_no == 14) { /* Page Fault: CR2 holds the faulting address */
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

        if (pmap_handle_fault(cr2, err_code)) {
            return; /* CoW break or stack growth resolved on the spot */
        }

        /* rudo-approved intentional fault -> the full MMIX PANIC. */
        {
            extern int g_rudo_fault_requester;
            if (g_rudo_fault_requester != 0) {
                int req = g_rudo_fault_requester;
                g_rudo_fault_requester = 0;
                task_t *rt = sched_find_pid(req);
                panic("Intentional page fault (rudo)\n"
                      "    requested by PID %d (%s), fault address 0x%lx",
                      req, rt ? rt->name : "?", (unsigned long)cr2);
            }
        }

        /* Unhandled user fault => SIGSEGV with default action. */
        task_t *cur = sched_get_current();
        if (cur != NULL && (frame->cs & 3) != 0) {
            kprintf("[SIG] Task '%s' (PID %d): page fault at 0x%x rip=0x%x -> SIGSEGV\n",
                    cur->name, cur->pid, cr2, frame->rip);
            cur->exit_status = -SIGSEGV;
            cur->zombie = true;
            task_t *parent = sched_find_pid(cur->parent_pid);
            if (parent != NULL) sched_wake(parent);
            sched_yield();
            /* Returns here only if nothing else is runnable. */
        }

        panic("Unhandled page fault (#14) at 0x%lx, err=0x%lx, rip=0x%lx",
              (unsigned long)cr2, (unsigned long)err_code,
              (unsigned long)frame->rip);
    }

    if (int_no < 32) {
        panic("CPU exception #%u (%s), err=0x%lx, rip=0x%lx",
              (uint32_t)int_no, exception_messages[int_no],
              (unsigned long)err_code, (unsigned long)frame->rip);
    } else if (int_no == TIMER_VECTOR && lapic_active()) {
        lapic_eoi();
        sched_timer_tick();
    } else if (int_no < IRQ_BASE + IRQ_COUNT) {
        pic_send_eoi((uint8_t)(int_no - IRQ_BASE));

        if (int_no == IRQ_BASE) { /* PIT: only used for LAPIC calibration */
            g_pit_ticks++;
        } else if (int_no == IRQ_BASE + 1) { /* PS/2 keyboard */
            kbd_interrupt();
        } else if (int_no == IRQ_BASE + 12) { /* PS/2 mouse */
            mouse_interrupt();
        }
    }
}

extern void idt_flush(uint64_t idtr_ptr);

void idt_init(void) {
    memset(&idt, 0, sizeof(idt));

    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64_t)&idt;

    for (int i = 0; i < 32; i++) {
        idt_set_gate((uint8_t)i, (uint64_t)exception_stubs[i],
                     GDT_KERNEL_CODE, IDT_KERNEL_GATE);
    }

    for (int i = 0; i < IRQ_COUNT; i++) {
        idt_set_gate((uint8_t)(IRQ_BASE + i), (uint64_t)irq_stubs[i],
                     GDT_KERNEL_CODE, IDT_KERNEL_GATE);
    }

    /* LAPIC spurious vector + LAPIC timer (vector 48, above the PIC) */
    extern void spurious_stub(void);
    extern void lapic_timer_stub(void);
    idt_set_gate(255, (uint64_t)spurious_stub, GDT_KERNEL_CODE, IDT_KERNEL_GATE);
    idt_set_gate(TIMER_VECTOR, (uint64_t)lapic_timer_stub, GDT_KERNEL_CODE,
                 IDT_KERNEL_GATE);

    idt_flush((uint64_t)&idtr);

    pic_remap(IRQ_BASE, IRQ_BASE + 8);

    kprintf("[IDT] Initialized, PIC remapped\n");
}
