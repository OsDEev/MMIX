/*
 * MBR partition table parser.
 * Reads partition tables and registers sub-block-devices with LBA offset.
 */
#include <blockdev.h>
#include <kheap.h>
#include <libk.h>
#include <partition.h>
#include <string.h>

#define MBR_SIZE 512

static struct part_info parts[32];
static int part_count = 0;

/* Per-partition read callback: translates LBA to parent + offset. */
static int part_read_cb(int dev_id, uint64_t lba, uint32_t count, void *buf) {
    if (dev_id < 0 || dev_id >= part_count) return -1;
    struct part_info *pi = &parts[dev_id];
    return blockdev_read(pi->parent_name, pi->lba_offset + lba, count, buf);
}

void partition_init(void) {
    part_count = 0;
    memset(parts, 0, sizeof(parts));

    int ndev = blockdev_count();
    kprintf("[MBR] Scanning %d block device(s)...\n", ndev);
    for (int d = 0; d < ndev; d++) {
        const struct blockdev *bd = blockdev_get_by_index(d);
        if (!bd) continue;
        kprintf("[MBR] Reading MBR from %s...\n", bd->name);

        /* Read MBR (sector 0) */
        uint8_t *mbr = kmalloc(MBR_SIZE);
        if (!mbr) { kprintf("[MBR] malloc failed\n"); continue; }
        int rc = blockdev_read(bd->name, 0, 1, mbr);
        kprintf("[MBR] Read returned %d\n", rc);
        if (rc != 1) {
            kfree(mbr);
            continue;
        }
        if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
            kfree(mbr);
            continue;
        }

        struct mbr_partition *tab = (struct mbr_partition *)(mbr + 446);
        for (int i = 0; i < MBR_PARTITION_COUNT && part_count < 32; i++) {
            struct mbr_partition *p = &tab[i];
            if (p->type == 0 || p->sectors == 0) continue;

            struct part_info *pi = &parts[part_count];
            strncpy(pi->parent_name, bd->name, sizeof(pi->parent_name) - 1);
            pi->parent_name[sizeof(pi->parent_name) - 1] = '\0';
            pi->part_num      = i + 1;
            pi->lba_offset    = p->lba_first;
            pi->total_sectors = p->sectors;
            pi->type          = p->type;
            pi->present       = true;

            /* Register partition as a block device with proper LBA-translating callback.
             * dev_id = index into our parts[] array. */
            char pname[16];
            snprintf(pname, sizeof(pname), "%s%d", bd->name, i + 1);
            blockdev_register(pname, part_count, p->sectors,
                              part_read_cb, NULL);

            kprintf("[MBR] %s type=0x%02x lba=%lu size=%lu%s\n",
                    pname, p->type, (unsigned long)p->lba_first,
                    (unsigned long)p->sectors,
                    (p->status & 0x80) ? " [boot]" : "");
            part_count++;
        }
        kfree(mbr);
    }
}

int partition_read_sectors(const char *part_name, uint64_t lba,
                           uint32_t count, void *buf) {
    /* Check registered partitions first */
    for (int i = 0; i < part_count; i++) {
        if (!parts[i].present) continue;
        char pname[16];
        snprintf(pname, sizeof(pname), "%s%d",
                 parts[i].parent_name, parts[i].part_num);
        if (strcmp(pname, part_name) == 0) {
            return blockdev_read(parts[i].parent_name,
                                 parts[i].lba_offset + lba, count, buf);
        }
    }
    /* No partition found — try raw block device (for whole-disk ext2) */
    return blockdev_read(part_name, lba, count, buf);
}
