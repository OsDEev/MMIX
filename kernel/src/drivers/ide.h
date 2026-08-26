/*
 * IDE PIO driver (legacy, port I/O based).
 * Reads/writes sectors via ports 0x1F0-0x1F7 (primary controller).
 * Supports LBA28 and LBA48 addressing.
 */
#ifndef MYUNIX_IDE_H
#define MYUNIX_IDE_H

#include <stdbool.h>
#include <stdint.h>

/* Primary IDE controller ports */
#define IDE_DATA       0x1F0
#define IDE_ERROR      0x1F1
#define IDE_SECTOR     0x1F2
#define IDE_LBA_LOW    0x1F3
#define IDE_LBA_MID    0x1F4
#define IDE_LBA_HIGH   0x1F5
#define IDE_DRIVE_HEAD 0x1F6
#define IDE_STATUS     0x1F7
#define IDE_COMMAND    0x1F7
#define IDE_CONTROL    0x3F6

/* Status register bits */
#define IDE_SR_BSY  0x80
#define IDE_SR_DRDY 0x40
#define IDE_SR_DRQ  0x08
#define IDE_SR_ERR  0x01
#define IDE_SR_DF   0x20

/* Commands */
#define IDE_CMD_READ_PIO_EXT  0x24
#define IDE_CMD_WRITE_PIO_EXT 0x34
#define IDE_CMD_IDENTIFY      0xEC

bool ide_init(void);
int  ide_read_sectors(int drive, uint64_t lba, uint32_t count, void *buf);
int  ide_write_sectors(int drive, uint64_t lba, uint32_t count, const void *buf);

#endif /* MYUNIX_IDE_H */
