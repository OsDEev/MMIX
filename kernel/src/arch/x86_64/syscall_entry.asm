global syscall_entry
extern syscall_handler
extern bsp_cpu

%define CPU_KSTACK_TOP 0x00
%define CPU_USER_RSP   0x08
%define CPU_USER_RIP   0x10

section .text
bits 64

; Syscall entry (IA32_LSTAR).
;
; On entry (ring 3):  rax = syscall number,
;                     rdi/rsi/rdx/r10/r8/r9 = arguments,
;                     rcx = return RIP, r11 = saved RFLAGS (by SYSCALL).
; IF is already cleared (IA32_FMASK).
;
; NOTE: no swapgs anywhere -- the per-cpu block is reached through the
; absolute address of bsp_cpu, so the GS pair can never get out of sync
; across task switches performed from interrupt context.
syscall_entry:
    push rcx             ; return RIP
    push r11             ; saved RFLAGS
    push rdi             ; [rsp+40]
    push rsi             ; [rsp+32]
    push rdx             ; [rsp+24]
    push r10             ; [rsp+16]
    push r8              ; [rsp+8]
    push r9              ; [rsp+0]

    mov rdi, rax         ; syscall number (rdi's user value is saved)

    ; Remember the user context (C code may patch it, see execve/signals).
    mov r11, bsp_cpu
    lea rax, [rsp + 64]
    mov [r11 + CPU_USER_RSP], rax
    mov [r11 + CPU_USER_RIP], rcx

    ; Marshal handler arguments while still addressing the user stack.
    mov rsi, [rsp + 40]  ; a1 <- user rdi
    mov rdx, [rsp + 32]  ; a2 <- user rsi
    mov rcx, [rsp + 24]  ; a3 <- user rdx
    mov r8,  [rsp + 16]  ; a4 <- user r10
    mov r9,  [rsp + 8]   ; a5 <- user r8
    mov rax, [rsp]       ; a6 <- user r9

    mov rsp, [r11 + CPU_KSTACK_TOP]
    push rax             ; a6: 7th parameter goes on the kernel stack
    call syscall_handler
    add rsp, 8           ; discard stack argument (rax = return value)

    ; Restore what SYSCALL clobbered
    mov rcx, [rsp + 56]
    mov r11, [rsp + 48]
    add rsp, 64

    ; Reload user context; C code may have patched it (execve/signals).
    mov rsi, bsp_cpu
    mov rsp, [rsi + CPU_USER_RSP]
    mov rcx, [rsi + CPU_USER_RIP]

    ; sysret needs REX.W to return to 64-bit user mode
    db 0x48
    sysret
