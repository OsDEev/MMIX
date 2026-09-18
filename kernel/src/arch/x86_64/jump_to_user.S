/* jump_to_user.S: build an iret frame and drop to ring 3.
 * GAS/AT&T version of the former NASM jump_to_user.asm.
 *
 * jump_to_user(rdi = entry, rsi = user_stack)
 *
 * NOTE: deliberately avoids touching RAX -- a fork()ed child enters user
 * mode through this path and must observe RAX==0 (the fork contract).
 */

.globl jump_to_user
.text
.code64

jump_to_user:
    movw $0x1B, %cx          /* user data selector (0x18 | RPL 3) */
    movw %cx, %ds
    movw %cx, %es
    movw %cx, %fs
    movw %cx, %gs

    pushq $0x1B              /* user SS */
    pushq %rsi               /* user RSP */
    pushq $0x202             /* RFLAGS with IF */
    pushq $0x23              /* user CS (0x20 | RPL 3) */
    pushq %rdi               /* user RIP */

    iretq