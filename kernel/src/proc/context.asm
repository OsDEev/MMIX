global context_switch

section .text
bits 64

context_switch:
    ; rdi = &old_rsp (or NULL), rsi = new_rsp

    test rdi, rdi
    jz .skip_save

    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp

.skip_save:
    mov rsp, rsi

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp

    ret