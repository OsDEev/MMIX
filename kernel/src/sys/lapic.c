#include <idt.h>
#include <io.h>
#include <lapic.h>
#include <libk.h>
#include <msr.h>
#include <pmap.h>

/*
 * Local APIC timer. Replaces the PIT as the scheduling tick; the PIT is
 * kept only briefly to calibrate the LAPIC counter and stays masked
 * afterwards (IRQ1/keyboard still uses the legacy PIC).
 */

#define MSR_APIC_BASE 0x1B

/* Register offsets from the LAPIC base */
#define LAPIC_ID        0x020
#define LAPIC_EOI       0x0B0
#define LAPIC_SPURIOUS  0x0F0
#define LAPIC_TIMER_LVT 0x320
#define LAPIC_INIT_CNT  0x380
#define LAPIC_CUR_CNT   0x390
#define LAPIC_DIVIDE    0x3E0

static volatile uint32_t *lapic = NULL;

static inline uint32_t lapic_read(uint32_t reg) {
    return lapic[reg / 4];
}

static inline void lapic_write(uint32_t reg, uint32_t val) {
    lapic[reg / 4] = val;
}

extern volatile uint64_t g_pit_ticks;

bool lapic_active(void) {
    return lapic != NULL;
}

void lapic_eoi(void) {
    if (lapic != NULL) {
        lapic_write(LAPIC_EOI, 0);
    }
}

static uint32_t calibrate_frequency(void) {
    const uint64_t target_ticks = 3; /* ~165 ms of PIT time */

    lapic_write(LAPIC_DIVIDE, 0xB);      /* divide by 16 */
    lapic_write(LAPIC_TIMER_LVT, 0x10000 | 0xFF); /* masked during calib */
    lapic_write(LAPIC_INIT_CNT, 0xFFFFFFFF);

    kprintf("[LAPIC] waiting for %u PIT ticks (have %u)...\n",
            (uint32_t)target_ticks, (uint32_t)g_pit_ticks);
    while (g_pit_ticks < target_ticks) {
        __asm__ volatile("pause");
    }
    kprintf("[LAPIC] PIT ticks arrived\n");

    uint32_t elapsed = 0xFFFFFFFFu - lapic_read(LAPIC_CUR_CNT);
    lapic_write(LAPIC_TIMER_LVT, 0x10000); /* mask */
    lapic_write(LAPIC_INIT_CNT, 0);

    /* PIT ticks at ~18.2065 Hz */
    uint64_t freq = (uint64_t)elapsed * 182065ULL / (target_ticks * 10000ULL);
    return (uint32_t)freq;
}

bool lapic_init(void) {
    uint64_t base_msr = rdmsr(MSR_APIC_BASE);
    kprintf("[LAPIC] APIC base MSR = 0x%x\n", base_msr);
    if ((base_msr & (1ULL << 11)) == 0) { /* not globally enabled */
        base_msr |= (1ULL << 11);
        wrmsr(MSR_APIC_BASE, base_msr);
    }
    uint64_t phys = base_msr & 0xFFFFF000ULL;

    pmap_map(kernel_pml4, LAPIC_VA, phys,
             PTE_WRITE | PTE_NOCACHE | PTE_PRESENT);
    lapic = (volatile uint32_t *)LAPIC_VA;

    kprintf("[LAPIC] mapped at 0x%x (phys 0x%x), id=%u\n",
            lapic, phys, lapic_read(LAPIC_ID));

    lapic_write(LAPIC_SPURIOUS, 0xFF | 0x100); /* enable + spurious vector */

    kprintf("[LAPIC] calibrating against PIT...\n");
    uint32_t freq = calibrate_frequency();
    if (freq == 0) return false;

    /* Mask the PIT channel-0 IRQ; the LAPIC timer takes over. */
    outb(0x21, (uint8_t)(inb(0x21) | 0x01));

    uint32_t init = freq / TIMER_HZ;
    lapic_write(LAPIC_DIVIDE, 0xB);              /* divide by 16 */
    lapic_write(LAPIC_TIMER_LVT, TIMER_VECTOR | 0x20000); /* periodic */
    lapic_write(LAPIC_INIT_CNT, init);

    kprintf("[LAPIC] Timer active: %u Hz core clock, tick=%u (~%d Hz)\n",
            freq, init, TIMER_HZ);
    return true;
}
