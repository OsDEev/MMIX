#include <gdt.h>
#include <libk.h>
#include <pmm.h>
#include <string.h>

/*
 * 64-bit GDT layout (8-byte slots; the TSS occupies two):
 *   0x00 null | 0x08 kcode | 0x10 kdata | 0x18 udata | 0x20 ucode | 0x28 TSS
 *
 * NOTE: user *data* must sit 8 bytes below user *code* because SYSRET
 * computes SS = STAR[63:48] + 8 and CS = STAR[63:48] + 16.  With the
 * opposite order (code below data) the returned user CS collapses onto
 * the TSS descriptor at 0x28, and the first IRETQ to the user task
 * raises #GP(err = 0x28).
 */
static uint64_t gdt[7];
static struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdtr;

/*
 * 64-bit TSS. Only RSP0 matters here: it is the stack the CPU switches to
 * when an interrupt arrives from ring 3.
 */
struct tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

static struct tss tss;

/* Dedicated stack for the double-fault handler (IST 1): it must survive
 * even when the current kernel stack pointer is garbage. pmm_alloc() returns
 * a direct-mapped (HHDM) address, so it is never dependent on the kernel
 * .bss mapping that might be incomplete when an early fault fires. */
static uint64_t df_stack_top = 0;

/*
 * Build one 8-byte descriptor byte-for-byte, matching the x86 memory
 * layout: limit_low, base_low, base_mid, access, gran, base_high.
 */
static uint64_t mk_desc(uint16_t limit_low, uint16_t base_low, uint8_t base_mid,
                        uint8_t access, uint8_t gran, uint8_t base_high) {
    return (uint64_t)limit_low |
           ((uint64_t)base_low << 16) |
           ((uint64_t)base_mid << 32) |
           ((uint64_t)access << 40) |
           ((uint64_t)gran << 48) |
           ((uint64_t)base_high << 56);
}

/* Flat code/data segment: base = 0, limits ignored in long mode. */
static void gdt_set_entry(int num, uint8_t access, uint8_t gran) {
    gdt[num] = mk_desc(0, 0, 0, access, gran, 0);
}

static void gdt_set_tss(int num, uint64_t base, uint16_t limit) {
    gdt[num] = mk_desc(limit,
                       (uint16_t)(base & 0xFFFF),
                       (uint8_t)((base >> 16) & 0xFF),
                       0x89,                    /* present, available 64-bit TSS */
                       (uint8_t)((limit >> 16) & 0xF),
                       (uint8_t)((base >> 24) & 0xFF));
    gdt[num + 1] = (uint32_t)(base >> 32);      /* base[63:32] */
}

extern void gdt_flush(uint64_t gdtr_ptr);

void tss_set_rsp0(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}

uint64_t tss_get_rsp0(void) {
    return tss.rsp0;
}

void gdt_init(void) {
    memset(&gdt, 0, sizeof(gdt));
    memset(&tss, 0, sizeof(tss));
    tss.iopb_offset = sizeof(tss);

    gdt_set_entry(0, 0x00, 0x00); /* null */
    gdt_set_entry(1, 0x9A, 0x20); /* kernel code: ring 0, L=1 */
    gdt_set_entry(2, 0x92, 0x00); /* kernel data: ring 0 */
    gdt_set_entry(3, 0xF2, 0x00); /* user data: ring 3 */
    gdt_set_entry(4, 0xFA, 0x20); /* user code: ring 3, L=1 */

    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base  = (uint64_t)&gdt;

    kprintf("[GDT] gdtr.base=0x%x, limit=%u\n", gdtr.base, gdtr.limit);
    kprintf("[GDT] Calling gdt_flush...\n");

    gdt_flush((uint64_t)&gdtr);

    gdt_set_tss(5, (uint64_t)&tss, sizeof(tss) - 1);
    df_stack_top = (uint64_t)pmm_alloc(1) + PAGE_SIZE;
    /* NOTE: the gate's IST index i refers to the CPU's slot i (at tss+0x24+i*8).
     * The C array is laid out so that ist[0] is the CPU's IST #1. */
    tss.ist[0] = df_stack_top;
    __asm__ volatile("ltr %%ax" :: "a"(GDT_TSS));

    kprintf("[GDT] Flush returned OK, TSS loaded\n");
}
