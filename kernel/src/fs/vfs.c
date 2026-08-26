#include <kheap.h>
#include <libk.h>
#include <string.h>
#include <vfs.h>

static vfs_node_t *root = NULL;

/* Provided by fs/procfs.c */
void procfs_mount(vfs_node_t *root_node);
int procfs_resolve(vfs_node_t *dir, const char *component, vfs_node_t **out);
int procfs_read(vfs_node_t *node, void *buf, size_t size, size_t offset);

/* TAR block size and ustar header (512 bytes) */
#define TAR_BLOCK 512

struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
} __attribute__((packed));

static size_t tar_parse_octal(const char *str, size_t len) {
    size_t val = 0;
    for (size_t i = 0; i < len && str[i] >= '0' && str[i] <= '7'; i++) {
        val = val * 8 + (str[i] - '0');
    }
    return val;
}

static vfs_node_t *vfs_create_node(const char *name, vfs_type_t type) {
    vfs_node_t *node = kcalloc(1, sizeof(vfs_node_t));
    if (node == NULL) return NULL;

    strncpy(node->name, name, VFS_MAX_NAME - 1);
    node->type = type;
    node->dev = NULL;

    return node;
}

/* Mount helpers ------------------------------------------------------- */

void vfs_mount_dev(const char *name, void *ops) {
    vfs_node_t *devdir = vfs_resolve("/dev");
    if (devdir == NULL) {
        vfs_node_t *root = vfs_resolve("/");
        if (root == NULL) return;
        devdir = vfs_create_node("dev", VFS_DIRECTORY);
        if (devdir == NULL) return;
        devdir->parent = root;
        devdir->next_sibling = root->children;
        root->children = devdir;
    }

    vfs_node_t *n = vfs_create_node(name, VFS_CHARDEV);
    if (n == NULL) return;
    n->dev = ops;
    n->parent = devdir;
    n->next_sibling = devdir->children;
    devdir->children = n;
}

void vfs_mount_procfs(void) {
    vfs_node_t *root = vfs_resolve("/");
    if (root == NULL) return;
    vfs_node_t *n = vfs_create_node("proc", VFS_PROCFS);
    if (n == NULL) return;
    n->parent = root;
    n->next_sibling = root->children;
    root->children = n;
    procfs_mount(n);
}

static vfs_node_t *vfs_get_or_create_dir(vfs_node_t *parent, const char *name) {
    /* Check if already exists */
    for (vfs_node_t *child = parent->children; child != NULL; child = child->next_sibling) {
        if (strcmp(child->name, name) == 0) {
            return child;
        }
    }

    vfs_node_t *dir = vfs_create_node(name, VFS_DIRECTORY);
    if (dir == NULL) return NULL;

    dir->parent = parent;
    dir->next_sibling = parent->children;
    parent->children = dir;

    return dir;
}

static vfs_node_t *vfs_ensure_path(vfs_node_t *start, char *path);

/* Public: create an empty file at `path`, creating parent directories. */
vfs_node_t *vfs_create_file(const char *path) {
    if (root == NULL || path == NULL || path[0] != '/') return NULL;

    char buf[VFS_MAX_PATH];
    strncpy(buf, path, VFS_MAX_PATH - 1);
    buf[VFS_MAX_PATH - 1] = '\0';

    char *last = strrchr(buf, '/');
    if (last == NULL) return NULL;

    vfs_node_t *parent = root;
    if (last != buf) {
        *last = '\0';
        parent = vfs_ensure_path(root, buf + 1);
        if (parent == NULL) return NULL;
    }

    /* Already exists? */
    for (vfs_node_t *c = parent->children; c; c = c->next_sibling) {
        if (strcmp(c->name, last + 1) == 0) return c;
    }

    vfs_node_t *node = vfs_create_node(last + 1, VFS_FILE);
    if (node == NULL) return NULL;
    node->parent = parent;
    node->next_sibling = parent->children;
    parent->children = node;
    return node;
}

/* Walk all leading path components of `path` (modified in place), creating dirs. */
static vfs_node_t *vfs_ensure_path(vfs_node_t *start, char *path) {
    vfs_node_t *current = start;
    char *token = path;
    char *sep;

    while ((sep = strchr(token, '/')) != NULL) {
        *sep = '\0';
        if (*token != '\0') {
            current = vfs_get_or_create_dir(current, token);
            if (current == NULL) return NULL;
        }
        token = sep + 1;
    }

    if (*token != '\0') {
        current = vfs_get_or_create_dir(current, token);
    }
    return current;
}

bool vfs_load_initrd(void *addr, size_t size) {
    if (root == NULL) {
        root = vfs_create_node("/", VFS_DIRECTORY);
        if (root == NULL) return false;
    }

    uint8_t *ptr = (uint8_t *)addr;
    uint8_t *end = ptr + size;
    int file_count = 0;

    while (ptr + sizeof(struct tar_header) <= end) {
        struct tar_header *hdr = (struct tar_header *)ptr;

        /* End of archive (two zero blocks) */
        if (hdr->name[0] == '\0') break;

        size_t file_size = tar_parse_octal(hdr->size, 12);
        char *name = hdr->name;

        /* Skip leading ./ */
        if (name[0] == '.' && name[1] == '/') {
            name += 2;
        }

        if (name[0] == '\0') {
            ptr += TAR_BLOCK + ((file_size + TAR_BLOCK - 1) & ~(size_t)(TAR_BLOCK - 1));
            continue;
        }

        uint8_t *data_start = ptr + TAR_BLOCK;
        char path[VFS_MAX_PATH];

        if (hdr->typeflag == '5' || name[strlen(name) - 1] == '/') {
            /* Directory: create every component */
            strncpy(path, name, VFS_MAX_PATH - 1);
            path[VFS_MAX_PATH - 1] = '\0';

            size_t plen = strlen(path);
            if (plen > 0 && path[plen - 1] == '/') path[plen - 1] = '\0';

            if (vfs_ensure_path(root, path) == NULL) return false;

        } else if (hdr->typeflag == '0' || hdr->typeflag == '\0') {
            /* Regular file */
            strncpy(path, name, VFS_MAX_PATH - 1);
            path[VFS_MAX_PATH - 1] = '\0';

            char *last_slash = strrchr(path, '/');
            char *filename;
            vfs_node_t *parent_dir = root;

            if (last_slash != NULL) {
                *last_slash = '\0';
                filename = last_slash + 1;
                parent_dir = vfs_ensure_path(root, path);
                if (parent_dir == NULL) return false;
            } else {
                filename = path;
            }

            vfs_node_t *file = vfs_create_node(filename, VFS_FILE);
            if (file != NULL) {
                file->data = kmalloc(file_size);
                if (file->data != NULL) {
                    memcpy(file->data, data_start, file_size);
                    file->size = file_size;
                    file->parent = parent_dir;
                    file->next_sibling = parent_dir->children;
                    parent_dir->children = file;
                    file_count++;
                }
            }
        }

        /* Advance to next entry (512-byte aligned) */
        ptr += TAR_BLOCK + ((file_size + TAR_BLOCK - 1) & ~(size_t)(TAR_BLOCK - 1));
    }

    kprintf("[VFS] Loaded %d files from initrd\n", file_count);
    return true;
}

void vfs_init(void) {
    root = vfs_create_node("/", VFS_DIRECTORY);
    if (root == NULL) {
        kprintf("[VFS] PANIC: cannot create root\n");
        return;
    }
    kprintf("[VFS] Initialized\n");
}

vfs_node_t *vfs_resolve(const char *path) {
    if (root == NULL) return NULL;
    if (path == NULL || path[0] != '/') return NULL;

    vfs_node_t *current = root;
    const char *p = path + 1; /* skip leading / */

    if (*p == '\0') return root;

    char component[VFS_MAX_NAME];

    while (*p != '\0') {
        /* Extract next path component */
        size_t i = 0;
        while (*p != '/' && *p != '\0' && i < VFS_MAX_NAME - 1) {
            component[i++] = *p++;
        }
        component[i] = '\0';

        if (*p == '/') p++;

        if (component[0] == '\0') continue;

        /* Dynamic filesystems */
        if (current->type == VFS_PROCFS) {
            vfs_node_t *out = NULL;
            if (procfs_resolve(current, component, &out) != 0) return NULL;
            current = out;
            continue;
        }

        /* Search children */
        vfs_node_t *found = NULL;
        for (vfs_node_t *child = current->children; child != NULL; child = child->next_sibling) {
            if (strcmp(child->name, component) == 0) {
                found = child;
                break;
            }
        }

        if (found == NULL) return NULL;
        current = found;
    }

    return current;
}

int vfs_read(vfs_node_t *node, void *buf, size_t size, size_t offset) {
    if (node == NULL) return -1;
    if (node->type == VFS_PROCFS) {
        return procfs_read(node, buf, size, offset);
    }
    if (node->type != VFS_FILE) return -1;
    if (offset >= node->size) return 0;

    size_t available = node->size - offset;
    size_t to_read = (size < available) ? size : available;

    memcpy(buf, node->data + offset, to_read);
    return (int)to_read;
}

/* RAM-backed files grow on demand. */
int vfs_write(vfs_node_t *node, const void *buf, size_t size, size_t offset) {
    if (node == NULL || node->type != VFS_FILE) return -1;

    size_t end = offset + size;
    if (end > node->size) {
        uint8_t *grown = krealloc(node->data, end);
        if (grown == NULL) return -1;
        /* Zero-fill any gap created by seeking past the old EOF. */
        if (offset > node->size) {
            memset(grown + node->size, 0, offset - node->size);
        }
        node->data = grown;
        node->size = end;
    }

    memcpy(node->data + offset, buf, size);
    return (int)size;
}
