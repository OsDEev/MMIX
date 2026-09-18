/*
 * crt0.S: SysV process entry.
 *   [rsp] = argc, [rsp+8] = argv[0..n], NULL, envp=NULL
 */
.section .text
.global _start
.extern main

_start:
    xor %rbp, %rbp

    mov (%rsp), %rdi          /* argc */
    lea 8(%rsp), %rsi         /* argv */
    and $-16, %rsp            /* align */

    call main                 /* exit_code = main(argc, argv) */

    mov %eax, %edi
    mov $0, %eax              /* SYS_EXIT */
    syscall

1:  jmp 1b