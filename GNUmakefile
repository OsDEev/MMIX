# MyUnix build (clang + lld)
# Requires: clang with lld (ld.lld), xorriso (iso),
#           qemu-system-x86_64 (run), tar (initrd), bear (compdb),
#           and a POSIX shell for find/sort/cp/tar (Linux, MSYS2, ...).
# Assembly is GAS/AT&T (.S), assembled by clang's integrated assembler.
#
# Targets:
#   make / make all   - build the kernel (bin/kernel)
#   make userspace    - build all userspace binaries (bin/...)
#   make initrd       - rebuild boot/initrd.tar from initrd_root/ (+ userspace)
#   make iso          - build the kernel and a bootable ISO
#   make run          - build ISO and boot it in QEMU (SeaBIOS)
#   make run-uefi     - build ISO and boot it in QEMU (OVMF)
#   make clean        - remove build artifacts
#   make distclean    - clean + remove IDE files
#   make compdb       - regenerate compile_commands.json via bear

.SUFFIXES:

# Keep intermediate object files (built via implicit rules) so that
# incremental builds don't rebuild everything on every run.
.SECONDARY:

override OUTPUT := kernel
ARCH := x86_64

# LLVM toolchain. If ld.lld is not on PATH, point CLANG_PREFIX at LLVM's
# bin/ directory, e.g.  make CLANG_PREFIX=/c/Program\ Files/LLVM/bin
CLANG_PREFIX ?=
CC := $(CLANG_PREFIX)clang
LD := $(CLANG_PREFIX)ld.lld

# Bare-metal x86-64 ELF target: no libc or target headers, System V ABI.
TARGET_TRIPLE := x86_64-unknown-none-elf
TARGET_FLAG := --target=$(TARGET_TRIPLE)

# === User-controllable flags ===
CFLAGS_USER := -g -O2 -pipe
LDFLAGS_USER :=

# === Internal kernel C/asm flags (DO NOT REMOVE) ===
override KCFLAGS := \
    $(CFLAGS_USER) \
    $(TARGET_FLAG) \
    -Wall -Wextra -Werror \
    -std=gnu11 \
    -ffreestanding \
    -fno-stack-protector \
    -fno-lto \
    -fno-PIC \
    -fno-pie \
    -ffunction-sections \
    -fdata-sections \
    -m64 \
    -march=x86-64 \
    -mno-80387 \
    -mno-mmx \
    -mno-sse \
    -mno-sse2 \
    -mno-red-zone \
    -mcmodel=kernel \
    -fno-builtin-memcpy \
    -fno-builtin-memset \
    -fno-builtin-memmove \
    -fno-builtin-memcmp \
    -fno-builtin-bzero \
    -fno-builtin-stpcpy

# === Internal CPP flags ===
override KCPPFLAGS := \
    -I kernel \
    -I kernel/src/lib \
    -I kernel/src/arch/x86_64 \
    -I kernel/src/mm \
    -I kernel/src/proc \
    -I kernel/src/fs \
    -I kernel/src/sys \
    -I kernel/src/drivers \
    -I kernel/src/drivers/usb \
    -MMD -MP

# === Internal linker flags ===
override KLDFLAGS := \
    $(LDFLAGS_USER) \
    -nostdlib \
    -static \
    -z max-page-size=0x1000 \
    --gc-sections \
    -T kernel/linker.ld

# === Userspace flags (ring 3 binaries, linked at low VA) ===
override UCFLAGS := $(CFLAGS_USER) $(TARGET_FLAG) -Wall -Wextra -std=gnu11 \
    -ffreestanding -fno-stack-protector -fno-pie -fno-lto \
    -mno-mmx -mno-sse -mno-sse2 -mno-80387
override ULDFLAGS := $(LDFLAGS_USER) -nostdlib -static -no-pie \
    -z max-page-size=0x1000 -T userspace/linker.ld

# === Source discovery ===
override CFILES := $(shell find kernel -name '*.c' 2>/dev/null | LC_ALL=C sort)
override ASFILES := $(shell find kernel -name '*.S' 2>/dev/null | LC_ALL=C sort)

override OBJ := \
    $(patsubst %.c,obj/%.c.o,$(CFILES)) \
    $(patsubst %.S,obj/%.S.o,$(ASFILES))

override DEPS := $(OBJ:.o=.d)

# === Default target ===
.PHONY: all
all: bin/$(OUTPUT)

# === Link kernel ===
bin/$(OUTPUT): $(OBJ) kernel/linker.ld
	@mkdir -p "$(dir $@)"
	$(LD) $(KLDFLAGS) $(OBJ) -o $@

# === Compile C (kernel) ===
obj/%.c.o: %.c
	@mkdir -p "$(dir $@)"
	$(CC) $(KCFLAGS) $(KCPPFLAGS) -c $< -o $@

# === Compile ASM (GAS/AT&T via clang's integrated assembler) ===
obj/%.S.o: %.S
	@mkdir -p "$(dir $@)"
	$(CC) $(KCFLAGS) $(KCPPFLAGS) -c $< -o $@

# === Userspace ===
USERSPACE_PROGS := init sh cat ls wc grep busy free fetch ps uptime date sleep reboot gfx panic rudod calc base64 sha256 echo clear pwd sound netinfo ping
USERSPACE_BINS := $(addprefix bin/,$(USERSPACE_PROGS))

.PHONY: userspace
userspace: $(USERSPACE_BINS)

# Shared objects (crt0, libc) are rebuilt per-program into private dirs.
obj/userspace/%/crt0.S.o: userspace/crt0.S
	@mkdir -p "$(dir $@)"
	$(CC) $(UCFLAGS) -c $< -o $@

obj/userspace/%/libc.c.o: userspace/libc.c
	@mkdir -p "$(dir $@)"
	$(CC) $(UCFLAGS) -c $< -o $@

obj/userspace/%/app.c.o: userspace/%.c
	@mkdir -p "$(dir $@)"
	$(CC) $(UCFLAGS) -c $< -o $@

bin/%: obj/userspace/%/crt0.S.o obj/userspace/%/app.c.o obj/userspace/%/libc.c.o userspace/linker.ld
	@mkdir -p "$(dir $@)"
	$(LD) $(ULDFLAGS) \
	    obj/userspace/$*/crt0.S.o obj/userspace/$*/app.c.o obj/userspace/$*/libc.c.o -o $@

# === Initrd ===
# initrd_root/ holds the source tree (etc/, tmp/); /bin/* is staged from
# the freshly built userspace binaries.
INITRD_FILES := $(shell find initrd_root -type f 2>/dev/null | LC_ALL=C sort)

.PHONY: initrd
initrd: boot/initrd.tar

boot/initrd.tar: $(INITRD_FILES) userspace
	@rm -rf obj/initrd
	@mkdir -p obj/initrd/bin
	@cp -r initrd_root/. obj/initrd/
	@cp $(USERSPACE_BINS) obj/initrd/bin/
	tar -cf $@ -C obj/initrd .
	@echo "initrd rebuilt: $@"

# === Include auto-generated deps ===
-include $(DEPS)

# === Build bootable ISO ===
.PHONY: iso
iso: bin/$(OUTPUT) boot/initrd.tar
	@mkdir -p iso_root/boot iso_root/EFI/BOOT
	@cp bin/$(OUTPUT) iso_root/boot/kernel.elf
	@cp boot/limine.conf iso_root/boot/
	@cp boot/initrd.tar iso_root/boot/
	@cp limine-binary/limine-bios.sys iso_root/boot/
	@cp limine-binary/limine-bios-cd.bin iso_root/boot/
	@cp limine-binary/limine-uefi-cd.bin iso_root/boot/
	@cp limine-binary/BOOTX64.EFI iso_root/EFI/BOOT/
	xorriso -as mkisofs \
	    -b boot/limine-bios-cd.bin \
	    -no-emul-boot -boot-load-size 4 -boot-info-table \
	    --efi-boot boot/limine-uefi-cd.bin \
	    -efi-boot-part --efi-boot-image \
	    --protective-msdos-label \
	    iso_root -o myunix.iso
	./limine-binary/limine bios-install myunix.iso
	@echo "ISO built: myunix.iso"

# === Run in QEMU ===
.PHONY: run
run: iso
	qemu-system-x86_64 -enable-kvm -m 512M -serial stdio \
	    -audiodev wav,id=snd0,path=audio-out.wav -device sb16,audiodev=snd0 \
	    -cdrom myunix.iso

.PHONY: run-uefi
run-uefi: iso
	qemu-system-x86_64 -enable-kvm -m 512M -serial stdio \
	    -audiodev wav,id=snd0,path=audio-out.wav -device sb16,audiodev=snd0 \
	    -bios /usr/share/OVMF/OVMF_CODE.fd -cdrom myunix.iso

# === Clean ===
.PHONY: clean
clean:
	rm -rf bin obj iso_root myunix.iso

.PHONY: distclean
distclean: clean
	rm -rf compile_commands.json

# === Generate compile_commands.json (for IDEs) ===
.PHONY: compdb
compdb:
	bear -- make all