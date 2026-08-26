/*
 * MBR partition table parser.
 * Reads partition tables from all registered block devices and creates
 * sub-block-devices like sda1, sda2 with proper LBA offset callbacks.
 */
#ifndef MYUNIX_PARTITION_H
#define MYUNIX_PARTITION_H

#include <stdbool.h>
#include <stdint.h>

#define MBR_PARTITION_COUNT 4

struct mbr_partition {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_first;
    uint32_t sectors;
};

/* Parsed partition info for internal use. */
struct part_info {
    char     parent_name[16];
    int      part_num;       /* 1..4 */
    uint32_t lba_offset;     /* first sector on parent */
    uint32_t total_sectors;
    uint8_t  type;
    bool     present;
};

void partition_init(void);
int  partition_read_sectors(const char *part_name, uint64_t lba,
                            uint32_t count, void *buf);

#endif /* MYUNIX_PARTITION_H */
