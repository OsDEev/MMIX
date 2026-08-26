/*
 * Block device registration and management.
 */
#include <blockdev.h>
#include <libk.h>
#include <string.h>

static struct blockdev devices[BLOCKDEV_MAX];
static int device_count = 0;

void blockdev_init(void) {
    memset(devices, 0, sizeof(devices));
    device_count = 0;
}

int blockdev_register(const char *name, int dev_id, uint64_t sectors,
                      block_read_fn read, block_write_fn write) {
    if (device_count >= BLOCKDEV_MAX) return -1;

    struct blockdev *d = &devices[device_count];
    strncpy(d->name, name, BLOCKDEV_NAME_MAX - 1);
    d->name[BLOCKDEV_NAME_MAX - 1] = '\0';
    d->dev_id   = dev_id;
    d->sectors  = sectors;
    d->read     = read;
    d->write    = write;
    d->present  = true;

    kprintf("[BLK] Registered %s (%lu sectors)\n", name, (unsigned long)sectors);
    return device_count++;
}

const struct blockdev *blockdev_get(const char *name) {
    for (int i = 0; i < device_count; i++) {
        if (devices[i].present && strcmp(devices[i].name, name) == 0)
            return &devices[i];
    }
    return NULL;
}

int blockdev_read(const char *name, uint64_t lba, uint32_t count, void *buf) {
    const struct blockdev *d = blockdev_get(name);
    if (!d || !d->read) return -1;
    return d->read(d->dev_id, lba, count, buf);
}

int blockdev_write(const char *name, uint64_t lba, uint32_t count, const void *buf) {
    const struct blockdev *d = blockdev_get(name);
    if (!d || !d->write) return -1;
    return d->write(d->dev_id, lba, count, buf);
}

int blockdev_count(void) { return device_count; }

const struct blockdev *blockdev_get_by_index(int i) {
    if (i < 0 || i >= device_count) return NULL;
    return &devices[i];
}
