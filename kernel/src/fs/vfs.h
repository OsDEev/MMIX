#ifndef MYUNIX_VFS_H
#define MYUNIX_VFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VFS_MAX_PATH 256
#define VFS_MAX_NAME 64

typedef enum {
    VFS_FILE,
    VFS_DIRECTORY,
    VFS_CHARDEV,  /* node->dev points at struct chardev */
    VFS_PROCFS    /* dynamic content; node->data encodes (pid<<8)|kind */
} vfs_type_t;

struct chardev;

typedef struct vfs_node {
    char name[VFS_MAX_NAME];
    vfs_type_t type;
    uint8_t *data;
    void *dev;          /* chardev ops for VFS_CHARDEV */
    size_t size;
    struct vfs_node *parent;
    struct vfs_node *children;
    struct vfs_node *next_sibling;
} vfs_node_t;

void vfs_init(void);
vfs_node_t *vfs_resolve(const char *path);
vfs_node_t *vfs_create_file(const char *path); /* mkdir -p + create */
void vfs_mount_dev(const char *name, void *ops);   /* /dev/<name> */
void vfs_mount_procfs(void);                       /* /proc */
int vfs_read(vfs_node_t *node, void *buf, size_t size, size_t offset);
int vfs_write(vfs_node_t *node, const void *buf, size_t size, size_t offset);
bool vfs_load_initrd(void *addr, size_t size);

#endif /* MYUNIX_VFS_H */
