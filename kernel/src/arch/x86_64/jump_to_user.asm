global jump_to_user

section .text
bits 64

; jump_to_user(rdi = entry, rsi = user_stack)
; Builds an iret frame and drops to ring 3.
;
; NOTE: deliberately avoids touching RAX -- a fork()ed child enters user
; mode through this path and must observe RAX==0 (the fork contract).
jump_to_user:
    mov cx, 0x23         ; user data selector (0x20 | RPL 3)
    mov ds, cx
    mov es, cx
    mov fs, cx
    mov gs, cx

    push qword 0x23      ; user SS
    push rsi             ; user RSP
    push qword 0x202     ; RFLAGS with IF
    push qword 0x1B      ; user CS (0x18 | RPL 3)
    push rdi             ; user RIP

    iretq
