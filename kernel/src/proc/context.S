/* context.S: save/restore callee-saved registers across a task switch.
 * GAS/AT&T version of the former NASM context.asm.
 *
 * context_switch(rdi = &old_rsp (or NULL), rsi = new_rsp)
 */

.globl context_switch
.text
.code64

context_switch:
    testq %rdi, %rdi
    jz .Lskip_save

    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, (%rdi)

.Lskip_save:
    movq %rsi, %rsp

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp

    retq