/*
 * IDE PIO driver.
 * Simple legacy port-I/O driver for QEMU's PIIX3 IDE controller.
 */
#include <blockdev.h>
#include <ide.h>
#include <io.h>
#include <kheap.h>
#include <libk.h>
#include <string.h>

/* Wait for BSY to clear with timeout. */
static bool ide_wait_bsy(void) {
    for (int i = 0; i < 100000; i++) {
        if (!(inb(IDE_STATUS) & IDE_SR_BSY)) return true;
    }
    return false;
}

/* Wait for DRQ or ERR. */
static bool ide_wait_drq(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t s = inb(IDE_STATUS);
        if (s & IDE_SR_ERR) return false;
        if (s & IDE_SR_DF) return false;
        if (s & IDE_SR_DRQ) return true;
    }
    return false;
}

/* Select drive (0=master, 1=slave) via drive/head register. */
static void ide_select_drive(uint8_t drive) {
    outb(IDE_DRIVE_HEAD, 0xA0 | (drive << 4));
    io_wait();
}

/* LBA48 read: READ SECTORS EXT (command 0x24). */
static bool ide_read_lba48(uint64_t lba, uint32_t count, void *buf) {
    uint8_t *dst = (uint8_t *)buf;

    /* If disk is small enough, use LBA28 (command 0x20) for simplicity */
    if (lba + count <= 0x0FFFFFFF && count <= 256) {
        ide_select_drive(0);
        if (!ide_wait_bsy()) { kprintf("[IDE] LBA28 wait bsy fail\n"); return false; }
        if (!(inb(IDE_STATUS) & IDE_SR_DRDY)) { kprintf("[IDE] LBA28 drdy fail\n"); return false; }

        outb(IDE_SECTOR, count & 0xFF);
        outb(IDE_LBA_LOW,  lba & 0xFF);
        outb(IDE_LBA_MID,  (lba >> 8) & 0xFF);
        outb(IDE_LBA_HIGH, (lba >> 16) & 0xFF);
        outb(IDE_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));
        outb(IDE_COMMAND, 0x20);

        for (uint32_t s = 0; s < count; s++) {
            if (!ide_wait_drq()) {
                kprintf("[IDE] LBA28 DRQ fail on sector %u, status=0x%x err=0x%x\n",
                        s, inb(IDE_STATUS), inb(IDE_ERROR));
                return false;
            }
            for (int i = 0; i < 256; i++) {
                uint16_t word = inw(IDE_DATA);
                dst[s * 512 + i * 2 + 0] = word & 0xFF;
                dst[s * 512 + i * 2 + 1] = (word >> 8) & 0xFF;
            }
        }
        return true;
    }

    /* LBA48 path */
    ide_select_drive(0);
    if (!ide_wait_bsy()) return false;
    if (!(inb(IDE_STATUS) & IDE_SR_DRDY)) return false;

    /* Write LBA48 sector count (high byte first) */
    outb(IDE_SECTOR, (count >> 8) & 0xFF);
    outb(IDE_LBA_LOW,  (lba >> 24) & 0xFF);
    outb(IDE_LBA_MID,  (lba >> 32) & 0xFF);
    outb(IDE_LBA_HIGH, (lba >> 40) & 0xFF);

    /* Write LBA48 sector count (low byte) and LBA low */
    outb(IDE_SECTOR, count & 0xFF);
    outb(IDE_LBA_LOW,  lba & 0xFF);
    outb(IDE_LBA_MID,  (lba >> 8) & 0xFF);
    outb(IDE_LBA_HIGH, (lba >> 16) & 0xFF);

    /* Send READ SECTORS EXT command */
    outb(IDE_COMMAND, IDE_CMD_READ_PIO_EXT);

    /* Read sectors */
    for (uint32_t s = 0; s < count; s++) {
        if (!ide_wait_drq()) return false;

        /* Read 256 words (512 bytes) */
        for (int i = 0; i < 256; i++) {
            uint16_t word = inw(IDE_DATA);
            dst[s * 512 + i * 2 + 0] = word & 0xFF;
            dst[s * 512 + i * 2 + 1] = (word >> 8) & 0xFF;
        }
    }

    return true;
}

/* LBA48 write: WRITE SECTORS EXT (command 0x34). */
static bool ide_write_lba48(uint64_t lba, uint32_t count, const void *buf) {
    const uint8_t *src = (const uint8_t *)buf;

    ide_select_drive(0);

    if (!ide_wait_bsy()) return false;
    if (!(inb(IDE_STATUS) & IDE_SR_DRDY)) return false;

    outb(IDE_SECTOR, (count >> 8) & 0xFF);
    outb(IDE_LBA_LOW,  (lba >> 24) & 0xFF);
    outb(IDE_LBA_MID,  (lba >> 32) & 0xFF);
    outb(IDE_LBA_HIGH, (lba >> 40) & 0xFF);

    outb(IDE_SECTOR, count & 0xFF);
    outb(IDE_LBA_LOW,  lba & 0xFF);
    outb(IDE_LBA_MID,  (lba >> 8) & 0xFF);
    outb(IDE_LBA_HIGH, (lba >> 16) & 0xFF);

    outb(IDE_COMMAND, IDE_CMD_WRITE_PIO_EXT);

    for (uint32_t s = 0; s < count; s++) {
        if (!ide_wait_drq()) return false;

        for (int i = 0; i < 256; i++) {
            uint16_t word = src[s * 512 + i * 2]
                          | ((uint16_t)src[s * 512 + i * 2 + 1] << 8);
            outw(IDE_DATA, word);
        }

        /* Flush cache */
        outb(IDE_COMMAND, 0xE7);
        if (!ide_wait_bsy()) return false;
    }

    return true;
}

/* IDENTIFY DEVICE: returns true if a drive exists at the given index. */
static bool ide_identify(uint8_t drive) {
    ide_select_drive(drive);

    if (!ide_wait_bsy()) return false;

    outb(IDE_SECTOR, 0);
    outb(IDE_LBA_LOW, 0);
    outb(IDE_LBA_MID, 0);
    outb(IDE_LBA_HIGH, 0);
    outb(IDE_COMMAND, IDE_CMD_IDENTIFY);

    if (!ide_wait_bsy()) return false;

    uint8_t status = inb(IDE_STATUS);
    if (status == 0) return false; /* No drive */

    if (!ide_wait_drq()) return false;

    /* Read 256 words of identify data */
    uint16_t ident[256];
    for (int i = 0; i < 256; i++) {
        ident[i] = inw(IDE_DATA);
    }

    /* Check for non-zero signature to confirm drive exists */
    if (ident[0] == 0) return false;

    /* Extract total LBA sectors (words 60-61 for LBA28, words 100-103 for LBA48) */
    uint32_t lba28_sectors = ident[60] | ((uint32_t)ident[61] << 16);
    uint64_t lba48_sectors = ((uint64_t)ident[100] | ((uint64_t)ident[101] << 16)
                             | ((uint64_t)ident[102] << 32) | ((uint64_t)ident[103] << 48));

    kprintf("[IDE] Drive %d: LBA28=%u LBA48=%lu sectors\n",
            drive, lba28_sectors, (unsigned long)lba48_sectors);
    return true;
}

/* Block device read callback. */
static int ide_block_read(int dev_id __attribute__((unused)), uint64_t lba, uint32_t count, void *buf) {
    if (ide_read_lba48(lba, count, buf)) return (int)count;
    kprintf("[IDE] Read failed: lba=%lu count=%u\n", (unsigned long)lba, count);
    return -1;
}

/* Block device write callback. */
static int ide_block_write(int dev_id __attribute__((unused)), uint64_t lba, uint32_t count, const void *buf) {
    if (ide_write_lba48(lba, count, buf)) return (int)count;
    return -1;
}

bool ide_init(void) {
    /* Probe for primary master drive */
    if (!ide_identify(0)) {
        kprintf("[IDE] No primary master drive found\n");
        return false;
    }

    /* Register as block device "sda" */
    blockdev_register("sda", 0, 0 /* size unknown for now */, ide_block_read, ide_block_write);
    return true;
}
