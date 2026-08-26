/*
 * Block device abstraction.
 * Each block device has a name, sector size, and read/write callbacks.
 */
#ifndef MYUNIX_BLOCKDEV_H
#define MYUNIX_BLOCKDEV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BLOCK_SECTOR_SIZE 512
#define BLOCKDEV_MAX      16
#define BLOCKDEV_NAME_MAX 16

typedef int (*block_read_fn)(int dev_id, uint64_t lba, uint32_t count, void *buf);
typedef int (*block_write_fn)(int dev_id, uint64_t lba, uint32_t count, const void *buf);

struct blockdev {
    char           name[BLOCKDEV_NAME_MAX];
    int            dev_id;      /* driver-specific identifier (e.g. AHCI port) */
    uint64_t       sectors;     /* total sectors on device */
    block_read_fn  read;
    block_write_fn write;       /* may be NULL for read-only */
    bool           present;
};

void blockdev_init(void);
int  blockdev_register(const char *name, int dev_id, uint64_t sectors,
                       block_read_fn read, block_write_fn write);
const struct blockdev *blockdev_get(const char *name);
int  blockdev_read(const char *name, uint64_t lba, uint32_t count, void *buf);
int  blockdev_write(const char *name, uint64_t lba, uint32_t count, const void *buf);
int  blockdev_count(void);
const struct blockdev *blockdev_get_by_index(int i);

#endif /* MYUNIX_BLOCKDEV_H */
