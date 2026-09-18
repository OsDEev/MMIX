/* gdt_flush.S: reload segment registers after loading a new GDT.
 * GAS/AT&T version of the former NASM gdt_flush.asm.
 */

.globl gdt_flush
.text
.code64

gdt_flush:
    /* rdi = pointer to GDT descriptor (limit + base) */
    lgdtq (%rdi)

    /* Far return to reload CS with our new GDT.
     * retfq pops: RIP from [rsp], CS from [rsp+8] */
    pushq $0x08                /* new CS (kernel code segment) */
    leaq .Lflush(%rip), %rax   /* address to return to */
    pushq %rax                 /* new RIP */
    lretq                      /* far return -> loads CS=0x08, RIP=.Lflush */

.Lflush:
    /* Reload data segments with kernel data selector */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    /* Clear FS and GS (will be set by scheduler later) */
    xorw %ax, %ax
    movw %ax, %fs
    movw %ax, %gs

    retq