/* syscall_entry.S: IA32_LSTAR entry, ring 3 -> kernel.
 * GAS/AT&T version of the former NASM syscall_entry.asm.
 *
 * On entry (ring 3):
 *   rax = syscall number, rdi/rsi/rdx/r10/r8/r9 = arguments,
 *   rcx = return RIP, r11 = saved RFLAGS (by SYSCALL).
 * IF is already cleared (IA32_FMASK).
 *
 * NOTE: no swapgs anywhere -- the per-cpu block is reached through the
 * absolute address of bsp_cpu, so the GS pair can never get out of sync
 * across task switches performed from interrupt context.
 */

.globl syscall_entry
.extern syscall_handler
.extern bsp_cpu

.equ CPU_KSTACK_TOP, 0x00
.equ CPU_USER_RSP,   0x08
.equ CPU_USER_RIP,   0x10
.equ CPU_USER_RFLAGS, 0x18

.text
.code64

syscall_entry:
    pushq %rcx             /* return RIP    */
    pushq %r11             /* saved RFLAGS  */
    pushq %rdi             /* [rsp+40]      */
    pushq %rsi             /* [rsp+32]      */
    pushq %rdx             /* [rsp+24]      */
    pushq %r10             /* [rsp+16]      */
    pushq %r8              /* [rsp+8]       */
    pushq %r9              /* [rsp+0]       */

    movq %rax, %rdi        /* syscall number (rdi's user value is saved) */

    /* Remember the user context (C code may patch it, see execve/signals). */
    movabsq $bsp_cpu, %r11
    leaq 64(%rsp), %rax
    movq %rax, CPU_USER_RSP(%r11)
    movq 48(%rsp), %rax    /* user RFLAGS (r11 slot) */
    movq %rax, CPU_USER_RFLAGS(%r11)
    movq %rcx, CPU_USER_RIP(%r11)

    /* Marshal handler arguments while still addressing the user stack. */
    movq 40(%rsp), %rsi    /* a1 <- user rdi */
    movq 32(%rsp), %rdx    /* a2 <- user rsi */
    movq 24(%rsp), %rcx    /* a3 <- user rdx */
    movq 16(%rsp), %r8     /* a4 <- user r10 */
    movq 8(%rsp), %r9      /* a5 <- user r8  */
    movq 0(%rsp), %rax     /* a6 <- user r9  */

    movq CPU_KSTACK_TOP(%r11), %rsp
    pushq %rax             /* a6: 7th parameter goes on the kernel stack */
    call syscall_handler
    addq $8, %rsp          /* discard stack argument (rax = return value) */

    /* Restore what SYSCALL clobbered from the per-CPU save area.
     * (The register pushes at entry live on the *user* stack, so reading
     * them relative to the swapped kernel stack would fetch garbage --
     * exactly how a corrupted R11 reached sysret and faulted.) */
    movabsq $bsp_cpu, %rsi
    movq CPU_USER_RSP(%rsi), %rsp
    movq CPU_USER_RIP(%rsi), %rcx
    movq CPU_USER_RFLAGS(%rsi), %r11

    /* sysret needs REX.W to return to 64-bit user mode */
    .byte 0x48
    sysret