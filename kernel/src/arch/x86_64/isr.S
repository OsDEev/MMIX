/* isr.S: exception/IRQ stubs, idt_flush, spurious and LAPIC timer stubs.
 * GAS/AT&T version of the former NASM isr.asm.
 */

.extern isr_handler

/* Interrupt stub without error code */
.macro ISR_NOERR num
    .globl isr\num
isr\num:
    pushq $0
    pushq $\num
    jmp isr_common_stub
.endm

/* Interrupt stub with error code */
.macro ISR_ERR num
    .globl isr\num
isr\num:
    pushq $\num
    jmp isr_common_stub
.endm

/* IRQ stub: vector is pushed explicitly (remapped vectors start at 32) */
.macro IRQ num vec
    .globl irq\num
irq\num:
    pushq $0
    pushq $\vec
    jmp isr_common_stub
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31

IRQ 0, 32
IRQ 1, 33
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

isr_common_stub:
    /* Save all general purpose registers */
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* Args: int_no, err_code, frame pointer */
    movq 120(%rsp), %rdi   /* int_no    */
    movq 128(%rsp), %rsi   /* err_code  */
    leaq 136(%rsp), %rdx   /* interrupt frame */

    call isr_handler

    /* Restore registers */
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    /* Remove int_no and err_code from stack */
    addq $16, %rsp

    iretq

.globl idt_flush
idt_flush:
    cli
    lidtq (%rdi)
    sti
    retq

/* Spurious LAPIC interrupt: acknowledge nothing, just return. */
.globl spurious_stub
spurious_stub:
    iretq

/* LAPIC timer (vector 48): same frame layout as the PIC stubs. */
.globl lapic_timer_stub
lapic_timer_stub:
    pushq $0
    pushq $48
    jmp isr_common_stub