#ifndef MYUNIX_IDT_H
#define MYUNIX_IDT_H

#include <stdint.h>

/* IRQ vectors are remapped to 32..47 by idt_init(). */
#define IRQ_BASE 32
#define IRQ_COUNT 16

void idt_init(void);
void idt_set_gate(uint8_t vector, uint64_t handler, uint16_t selector, uint8_t flags);

/* C-side dispatcher, called from the common stub in isr.asm */
struct interrupt_frame {
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};
void isr_handler(uint64_t int_no, uint64_t err_code, struct interrupt_frame *frame);

/* Exception stubs (isr.asm) */
void isr0(void);  void isr1(void);  void isr2(void);  void isr3(void);
void isr4(void);  void isr5(void);  void isr6(void);  void isr7(void);
void isr8(void);  void isr9(void);  void isr10(void); void isr11(void);
void isr12(void); void isr13(void); void isr14(void); void isr15(void);
void isr16(void); void isr17(void); void isr18(void); void isr19(void);
void isr20(void); void isr21(void); void isr22(void); void isr23(void);
void isr24(void); void isr25(void); void isr26(void); void isr27(void);
void isr28(void); void isr29(void); void isr30(void); void isr31(void);

/* IRQ stubs (isr.asm) */
void irq0(void);  void irq1(void);  void irq2(void);  void irq3(void);
void irq4(void);  void irq5(void);  void irq6(void);  void irq7(void);
void irq8(void);  void irq9(void);  void irq10(void); void irq11(void);
void irq12(void); void irq13(void); void irq14(void); void irq15(void);

#endif /* MYUNIX_IDT_H */
