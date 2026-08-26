global gdt_flush

section .text
bits 64

gdt_flush:
    ; rdi = pointer to GDT descriptor (limit + base)
    lgdt [rdi]

    ; Far return to reload CS with our new GDT
    ; retfq pops: RIP from [rsp], CS from [rsp+8]
    push 0x08                ; new CS (kernel code segment)
    lea rax, [rel .flush]   ; address to return to
    push rax                 ; new RIP
    retfq                    ; far return -> loads CS=0x08, RIP=.flush

.flush:
    ; Reload data segments with kernel data selector
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Clear FS and GS (will be set by scheduler later)
    xor ax, ax
    mov fs, ax
    mov gs, ax

    ret
