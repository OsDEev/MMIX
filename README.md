# MMix

A hobby x86_64 operating system bootable via the
[Limine boot protocol](https://github.com/limine-bootloader/limine).

**Current version: 0.4.0**

## Features

### Kernel
- Limine boot protocol (base revision 3), HHDM + memory map
- Physical memory manager (bitmap allocator), kernel heap
- Per-process page tables (CR3 switch), full address-space teardown,
  frame refcounting, automatic user-stack growth, `mmap`/`munmap`
- GDT with TSS (RSP0 per task), ring 3 usermode, IDT with full
  exception/IRQ stubs
- LAPIC timer preemption (calibrated against the PIT); PIC kept only
  for the keyboard IRQ
- Preemptive round-robin scheduler with blocked/zombie states
- Syscalls via `syscall`/`sysret` with per-task kernel stacks (33 syscalls)
- Processes: `fork`, `execve` (SysV argc/argv), `waitpid`, `exit`,
  `getpid`, `getppid`, `getuid`, `setuid`; orphans reparented to init

### Signals & Process Groups
- `kill`, `signal`, `sigreturn`; Ctrl+C raises SIGINT,
  unhandled page faults raise SIGSEGV, SIG_IGN/SIG_DFL/handlers with
  a sigreturn trampoline on the user stack
- Process groups & sessions: `setpgid`/`getpgid`/`setsid`/`tcsetpgrp`;
  Ctrl+C delivers SIGINT to the console foreground group only

### Filesystem & IPC
- File descriptors per process: `open` (with O_CREATE/O_APPEND),
  `close`, `read`, `write`, `dup`, `dup2`, `pipe`, `brk`
- Anonymous pipes with blocking semantics, EOF and EPIPE
- RAM-backed writable VFS (tar initrd) with file creation
- **devfs**: /dev/console, /dev/null, /dev/zero, /dev/random
- **procfs**: /proc/meminfo, /proc/uptime, /proc/stat,
  /proc/\<pid\>/status, /proc/\<pid\>/cmdline

### Drivers
- PS/2 keyboard (Ctrl+C aware), framebuffer text console
  (8x8 font, scrolling, cursor; ANSI CSI parser with SGR colors)
- RTC via CMOS, LAPIC timer, COM1 serial mirror
- Graphics primitives: rect, line, circle, fill (via SYS_GFX)

### Privilege Separation (v0.4.0)
- **rudo** system (root user do): unprivileged processes request
  privileged operations through the `rudod` daemon (PID 2, uid 0)
- `rudo_request` blocks until rudod approves via `rudo_wait`
- `RUDO_OP_PANIC`: approved request triggers an intentional kernel
  page fault, rendering the full-screen **kernel panic** (BSOD)
- `RUDO_OP_SETUID`: approved request sets the requester's uid to 0

### Userspace
- Interactive `/bin/sh` with pipelines (`a | b | c`), I/O redirection
  (`>`, `>>`, `<`), and builtins: `echo`, `help`, `exit`, `pid`, `ppid`,
  `kill`, `whoami`, `id`, `root`, `rudo`
- `root` command: escalates shell to uid 0 via rudo/rudod
- `rudo <cmd>`: runs a command as root (like sudo)
- Programs: `init`, `sh`, `cat`, `ls`, `wc`, `grep`, `busy`, `free`,
  `fetch` (colored system info), `ps`, `uptime`, `date`, `sleep`,
  `reboot`, `gfx` (graphics demo), `panic` (BSOD trigger), `rudod`,
  `desktop` (graphical interface, BETA)
- libc with `malloc`/`calloc`/`free` over mmap, string functions,
  `print`/`print_num` helpers

### Graphics Interface (BETA)
- `desktop` command renders a graphical desktop environment with:
  - Dark background with taskbar
  - App icon grid with colored accents
  - System info widgets (memory bar, clock)
  - Keyboard-driven: press 1-8 to launch demo screens, ESC exits

## Layout

```
kernel/src/
  main.c               entry point and boot sequence
  lib/                 kprintf/serial, panic (BSOD screen), freestanding <string.h>
  arch/x86_64/         GDT/TSS, IDT/ISR stubs, io/msr/cpu helpers,
                       syscall entry, jump-to-usermode
  drivers/             PS/2 keyboard, framebuffer TTY, font, devfs, gfx, RTC
  mm/                  PMM, kernel heap, page tables (pmap)
  proc/                scheduler, ELF loader/exec images
  fs/                  tar initrd, in-memory VFS, pipes, procfs
  sys/                 syscall table (33 calls), signals, LAPIC timer, rudo
userspace/             crt0, libc, init, sh, cat, ls, wc, grep, busy,
                       free, fetch, ps, uptime, date, sleep, reboot,
                       gfx, panic, rudod, desktop
initrd_root/           initrd source tree (etc/, tmp/)
```

## Building

```sh
make            # kernel
make userspace  # all userland binaries
make initrd     # repack boot/initrd.tar
make iso        # bootable ISO
make run        # boot in QEMU (KVM)
make clean
```

## Status / known limitations

- `fork()` copies address spaces eagerly. The CoW sharing path exists
  (PTE soft bit, refcounts, #PF break) but is disabled pending a race
  investigation with timer preemption.
- Signals are delivered at syscall entry; a CPU-bound loop without
  syscalls only notices Ctrl+C when it yields. Handlers receive no
  arguments; SIGSEGV handlers are not supported (default terminates).
- Single CPU. LAPIC work is the groundwork for future AP bring-up.
- Filesystem is the RAM-backed initrd (writable, not persisted).
  No block devices yet: AHCI + Ext2 is the next milestone.
- No PTY layer: single shared console with one foreground process group.
- `desktop` (BETA): keyboard-driven only, no mouse support.
  Text rendering in gfx modes limited to colored rectangles.
- `rudo` auto-approves SETUID requests. Full authentication
  (password/sudoers) is planned.
