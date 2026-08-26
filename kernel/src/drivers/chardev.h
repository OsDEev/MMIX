#ifndef MYUNIX_CHARDEV_H
#define MYUNIX_CHARDEV_H

#include <stddef.h>

/* Character device operations; every device is a byte stream. */
struct chardev {
    const char *name;
    int (*read)(char *buf, size_t n);           /* return bytes read, 0=EOF, -1 err */
    int (*write)(const char *buf, size_t n);    /* return bytes written */
};

void devfs_init(void);

#endif /* MYUNIX_CHARDEV_H */
