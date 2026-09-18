#!/bin/bash
# Build MMix and boot it in QEMU with the boot log shown on the console.
# Exit QEMU: Ctrl-A X   Toggle to QEMU monitor: Ctrl-A C   Help: Ctrl-A H
set -e
cd /mnt/c/Users/Neeil/MMIX

touch kernel/src/arch/x86_64/syscall_entry.S kernel/src/arch/x86_64/cpu.h \
      kernel/src/proc/sched.c kernel/src/proc/sched.h kernel/src/sys/syscall.c
make 2>&1 | grep -E "error|Error" | head -20
if ! make iso >/dev/null 2>&1; then
    echo "ISO_FAIL"
    exit 1
fi
echo "ISO_OK"

exec qemu-system-x86_64 -m 512M -no-reboot -nographic \
     -device piix4-usb-uhci -device usb-kbd \
     -audiodev wav,id=snd0,path=/tmp/mmix-audio.wav -device sb16,audiodev=snd0 \
     -cdrom myunix.iso