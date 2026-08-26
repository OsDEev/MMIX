# MyUnix

A small hobby operating system for x86_64, bootable via the
[Limine boot protocol](https://github.com/limine-bootloader/limine).

## Features

- Limine boot protocol (base revision 3), HHDM + memory map
- Physical memory manager (bitmap allocator), kernel heap
- Per-process page tables (CR3 switch), full address-space teardown,
  frame refcounting, automatic user-stack growth, `mmap`/`munmap`
- GDT with TSS (RSP0 per task), ring 3 usermode, IDT with full
  exception/IRQ stubs
- LAPIC timer preemption (calibrated against the PIT); PIC kept only
  for the keyboard IRQ
- Preemptive round-robin scheduler with blocked/zombie states
- Syscalls via `syscall`/`sysret` with per-task kernel stacks
- Processes: `fork`, `execve` (SysV argc/argv), `waitpid`, `exit`,
  `getpid`, `getppid`; orphans reparented to init
- Signals: `kill`, `signal`, `sigreturn`; Ctrl+C raises SIGINT,
  unhandled page faults raise SIGSEGV, SIG_IGN/SIG_DFL/handlers with
  a sigreturn trampoline on the user stack
- File descriptors per process: `open` (with O_CREATE/O_APPEND),
  `close`, `read`, `write`, `dup`, `dup2`, `pipe`, `brk`
- Anonymous pipes with blocking semantics, EOF and EPIPE
- RAM-backed writable VFS (tar initrd) with file creation
- **devfs**: /dev/console, /dev/null, /dev/zero, /dev/random
- **procfs**: /proc/meminfo, /proc/uptime, /proc/stat,
  /proc/<pid>/status, /proc/<pid>/cmdline (ps/uptime read these)
- Process groups & sessions: setpgid/getpgid/setsid/tcsetpgrp;
  Ctrl+C delivers SIGINT to the console foreground group only
- Drivers: PS/2 keyboard (Ctrl+C aware), framebuffer text console
  (8x8 font, scrolling, cursor; mirrored to COM1)
- Userland: interactive `/bin/sh` with pipelines (`a | b | c`) and
  I/O redirection (`>`, `>>`, `<`), plus `init`, `cat`, `ls`, `wc`,
  `grep`, `busy`, `free`, `fetch`, `ps`, `uptime`; libc with
  malloc/calloc/free over mmap

## Layout

```
kernel/src/
  main.c               entry point and boot sequence
  lib/                 kprintf/serial console, freestanding <string.h>
  arch/x86_64/         GDT/TSS, IDT/ISR stubs, io/msr/cpu helpers,
                       syscall entry, jump-to-usermode
  drivers/             PS/2 keyboard, framebuffer TTY, font
  mm/                  PMM, kernel heap, page tables (pmap)
  proc/                scheduler, ELF loader/exec images
  fs/                  tar initrd, in-memory VFS, pipes
  sys/                 syscall table, signals, LAPIC timer
userspace/             crt0, libc, init, sh, cat, ls, wc, grep, busy
initrd_root/           initrd source tree (etc/, tmp/)
```

## Building

```sh
make            # kernel
make iso        # bootable ISO (uses committed boot/initrd.tar)
make run        # boot in QEMU
make userspace  # build all userland binaries
make initrd     # repack boot/initrd.tar after userspace/initrd_root changes
make clean
```

## Status / known limitations

- `fork()` copies address spaces eagerly. The CoW sharing path exists
  (PTE soft bit, refcounts, #PF break) but is disabled pending a race
  investigation with timer preemption.
- Signals are delivered at syscall entry (and from the page-fault
  path); a CPU-bound loop without syscalls only notices Ctrl+C when it
  yields. Handlers receive no arguments; SIGSEGV handlers are not
  supported (default action terminates).
- Single CPU. LAPIC work is the groundwork for future AP bring-up.
  Preemption is request-based: the timer tick sets a flag and the
  switch happens at the next syscall boundary (ISR-context switching
  is being redesigned).
- Filesystem is the RAM-backed initrd (files writable, contents not
  persisted). No block devices yet: AHCI + Ext2 is the next milestone.
- No PTY layer yet: the console is a single shared tty with one
  foreground process group.
