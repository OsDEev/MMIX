/*
 * Ext2 filesystem driver (read-only).
 * Reads superblock, block group descriptors, inodes, directories.
 * Provides file read and directory listing.
 */
#include <ext2.h>
#include <kheap.h>
#include <libk.h>
#include <partition.h>
#include <string.h>

/* Mounted filesystem state (single mount for now) */
static struct {
    char dev_name[16];
    struct ext2_superblock sb;
    struct ext2_bgd *bgd;        /* block group descriptor table */
    uint32_t block_size;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t bgd_count;
    bool     mounted;
} ext2;

/* ---- Low-level disk I/O via partition layer ---- */

static int read_block(uint32_t block, void *buf) {
    uint32_t sectors = ext2.block_size / 512;
    return partition_read_sectors(ext2.dev_name, (uint64_t)block * sectors,
                                  sectors, buf);
}

static int read_blocks(uint32_t start, uint32_t count, void *buf) {
    uint32_t sectors_per = ext2.block_size / 512;
    uint8_t *p = (uint8_t *)buf;
    for (uint32_t i = 0; i < count; i++) {
        if (read_block(start + i, p + i * ext2.block_size) != (int)sectors_per)
            return -1;
    }
    return 0;
}

/* ---- Superblock / BGD init ---- */

static bool ext2_read_superblock(void) {
    uint8_t *buf = kmalloc(1024);
    if (!buf) return false;

    /* Superblock is at byte offset 1024 from partition start (block 0 partial + block 1) */
    if (partition_read_sectors(ext2.dev_name, 2, 2, buf) != 2) {
        kfree(buf);
        return false;
    }

    memcpy(&ext2.sb, buf + 0, sizeof(ext2.sb));
    kfree(buf);

    if (ext2.sb.s_magic != EXT2_SUPER_MAGIC) {
        kprintf("[EXT2] Bad magic: 0x%04x (expected 0xEF53)\n", ext2.sb.s_magic);
        return false;
    }

    ext2.block_size    = 1024u << ext2.sb.s_log_block_size;
    ext2.blocks_per_group = ext2.sb.s_blocks_per_group;
    ext2.inodes_per_group = ext2.sb.s_inodes_per_group;
    ext2.inode_size    = ext2.sb.s_inode_size ? ext2.sb.s_inode_size : 128;
    ext2.bgd_count     = (ext2.sb.s_blocks_count + ext2.blocks_per_group - 1)
                         / ext2.blocks_per_group;

    kprintf("[EXT2] v%d.%d block_size=%u blocks=%u inodes=%u bgds=%u\n",
            ext2.sb.s_rev_level, ext2.sb.s_minor_rev_level,
            ext2.block_size, ext2.sb.s_blocks_count,
            ext2.sb.s_inodes_count, ext2.bgd_count);
    return true;
}

static bool ext2_read_bgd(void) {
    uint32_t bgd_blocks = (ext2.bgd_count * sizeof(struct ext2_bgd) + ext2.block_size - 1)
                          / ext2.block_size;
    uint32_t bgd_start = ext2.sb.s_first_data_block + 1;

    ext2.bgd = kmalloc(bgd_blocks * ext2.block_size);
    if (!ext2.bgd) return false;

    if (read_blocks(bgd_start, bgd_blocks, ext2.bgd) != 0) {
        kfree(ext2.bgd);
        ext2.bgd = NULL;
        return false;
    }

    return true;
}

/* ---- Inode I/O ---- */

static bool read_inode(uint32_t inode_no, struct ext2_inode *out) {
    if (inode_no == 0) return false;
    inode_no--;  /* inodes are 1-based */

    uint32_t group = inode_no / ext2.inodes_per_group;
    uint32_t index = inode_no % ext2.inodes_per_group;

    if (group >= ext2.bgd_count) return false;

    struct ext2_bgd *bg = &ext2.bgd[group];
    uint32_t inode_table_block = bg->bg_inode_table;
    uint64_t offset = (uint64_t)index * ext2.inode_size;
    uint32_t block_in_table = (uint32_t)(offset / ext2.block_size);
    uint32_t off_in_block   = (uint32_t)(offset % ext2.block_size);

    uint8_t *block_buf = kmalloc(ext2.block_size);
    if (!block_buf) return false;

    if (read_block(inode_table_block + block_in_table, block_buf) != (int)(ext2.block_size / 512)) {
        kfree(block_buf);
        return false;
    }

    memcpy(out, block_buf + off_in_block, sizeof(struct ext2_inode));
    kfree(block_buf);
    return true;
}

/* ---- Block reading (direct, indirect, double-indirect) ---- */

static uint32_t get_inode_block(const struct ext2_inode *inode, uint32_t logical) {
    uint32_t ptrs_per_block = ext2.block_size / 4;
    uint32_t direct_count = 12;

    if (logical < direct_count) {
        return inode->i_block[logical];
    }
    logical -= direct_count;

    /* Single indirect */
    if (logical < ptrs_per_block) {
        uint32_t *indirect = kmalloc(ext2.block_size);
        if (!indirect) return 0;
        if (read_block(inode->i_block[12], indirect) != (int)(ext2.block_size / 512)) {
            kfree(indirect);
            return 0;
        }
        uint32_t block = indirect[logical];
        kfree(indirect);
        return block;
    }
    logical -= ptrs_per_block;

    /* Double indirect */
    uint32_t di_idx = logical / ptrs_per_block;
    uint32_t di_off = logical % ptrs_per_block;

    uint32_t *dind = kmalloc(ext2.block_size);
    if (!dind) return 0;
    if (read_block(inode->i_block[13], dind) != (int)(ext2.block_size / 512)) {
        kfree(dind);
        return 0;
    }
    uint32_t next_block = dind[di_idx];
    kfree(dind);
    if (next_block == 0) return 0;

    uint32_t *indirect = kmalloc(ext2.block_size);
    if (!indirect) return 0;
    if (read_block(next_block, indirect) != (int)(ext2.block_size / 512)) {
        kfree(indirect);
        return 0;
    }
    uint32_t block = indirect[di_off];
    kfree(indirect);
    return block;
}

/* Read `count` bytes at offset from an inode into buf. Returns bytes read. */
static int read_inode_data(const struct ext2_inode *inode,
                           uint64_t offset, uint32_t count, void *buf) {
    uint32_t file_size = inode->i_size;
    if ((uint32_t)offset >= file_size) return 0;
    if (offset + count > file_size) count = file_size - (uint32_t)offset;

    uint8_t *dst = (uint8_t *)buf;
    uint32_t bytes_read = 0;

    while (count > 0) {
        uint32_t block_num = (uint32_t)(offset / ext2.block_size);
        uint32_t off_in_block = (uint32_t)(offset % ext2.block_size);
        uint32_t to_copy = ext2.block_size - off_in_block;
        if (to_copy > count) to_copy = count;

        uint32_t phys_block = get_inode_block(inode, block_num);
        if (phys_block == 0) {
            memset(dst, 0, to_copy);
        } else {
            uint8_t *block_buf = kmalloc(ext2.block_size);
            if (!block_buf) return bytes_read;
            if (read_block(phys_block, block_buf) != (int)(ext2.block_size / 512)) {
                kfree(block_buf);
                return bytes_read;
            }
            memcpy(dst, block_buf + off_in_block, to_copy);
            kfree(block_buf);
        }

        dst       += to_copy;
        offset    += to_copy;
        count     -= to_copy;
        bytes_read += to_copy;
    }
    return (int)bytes_read;
}

/* ---- Directory lookup ---- */

struct dir_iter {
    uint32_t inode_no;
    const char *name;
    uint8_t  file_type;
    bool     found;
};

static int dir_lookup_callback(uint32_t dir_ino, struct dir_iter *iter) {
    struct ext2_inode dinode;
    if (!read_inode(dir_ino, &dinode)) return -1;
    if ((dinode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;

    uint64_t offset = 0;
    uint32_t size = dinode.i_size;

    while (offset < size) {
        struct ext2_dirent ent;
        if (read_inode_data(&dinode, offset, sizeof(ent), &ent) != sizeof(ent))
            break;
        if (ent.inode == 0) {
            offset += ent.rec_len;
            continue;
        }

        char name_buf[256];
        uint32_t name_len = ent.name_len;
        if (name_len > 255) name_len = 255;
        if (read_inode_data(&dinode, offset + sizeof(struct ext2_dirent),
                            name_len, name_buf) != (int)name_len)
            break;
        name_buf[name_len] = '\0';

        if (strcmp(name_buf, iter->name) == 0) {
            iter->inode_no = ent.inode;
            iter->file_type = ent.file_type;
            iter->found = true;
            return 0;
        }

        offset += ent.rec_len;
    }
    return 0;
}

/* Resolve a path to an inode number. Returns 0 on failure. */
static uint32_t resolve_path(const char *path) {
    if (path[0] != '/') return 0;
    path++;  /* skip leading / */

    uint32_t current_ino = EXT2_ROOT_INO;

    while (*path) {
        while (*path == '/') path++;
        if (*path == '\0') break;

        /* Extract component */
        char component[256];
        int i = 0;
        while (*path && *path != '/' && i < 255) {
            component[i++] = *path++;
        }
        component[i] = '\0';

        if (strcmp(component, ".") == 0) continue;
        if (strcmp(component, "..") == 0) {
            /* Go to parent: read dir entry "." of current to find parent inode.
             * For simplicity, just use root if we're at root's children. */
            struct ext2_inode ci;
            if (!read_inode(current_ino, &ci)) return 0;
            /* We don't store parent inodes; use root as fallback */
            current_ino = EXT2_ROOT_INO;
            continue;
        }

        struct dir_iter iter = {
            .inode_no = 0,
            .name = component,
            .file_type = 0,
            .found = false
        };
        dir_lookup_callback(current_ino, &iter);
        if (!iter.found) return 0;
        current_ino = iter.inode_no;
    }
    return current_ino;
}

/* ---- Directory listing ---- */

/* Allocate and populate VFS directory entries. */
struct ext2_dir_entry {
    char     name[64];
    uint32_t inode_no;
    uint8_t  file_type;
};

static int ext2_list_dir_internal(const char *path, struct ext2_dir_entry **out_entries, int *out_count) {
    uint32_t ino = resolve_path(path);
    if (ino == 0) return -1;

    struct ext2_inode dinode;
    if (!read_inode(ino, &dinode)) return -1;
    if ((dinode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;

    /* Count entries first */
    int count = 0;
    uint64_t off = 0;
    uint32_t size = dinode.i_size;
    while (off < size) {
        struct ext2_dirent ent;
        if (read_inode_data(&dinode, off, sizeof(ent), &ent) != sizeof(ent)) break;
        if (ent.inode != 0) count++;
        off += ent.rec_len;
    }

    struct ext2_dir_entry *entries = kmalloc(count * sizeof(struct ext2_dir_entry));
    if (!entries) return -1;

    int idx = 0;
    off = 0;
    while (off < size && idx < count) {
        struct ext2_dirent ent;
        if (read_inode_data(&dinode, off, sizeof(ent), &ent) != sizeof(ent)) break;
        if (ent.inode == 0) { off += ent.rec_len; continue; }

        entries[idx].inode_no = ent.inode;
        entries[idx].file_type = ent.file_type;
        uint32_t nlen = ent.name_len > 63 ? 63 : ent.name_len;
        read_inode_data(&dinode, off + sizeof(struct ext2_dirent),
                        nlen, entries[idx].name);
        entries[idx].name[nlen] = '\0';
        idx++;
        off += ent.rec_len;
    }

    *out_entries = entries;
    *out_count = idx;
    return 0;
}

/* ---- Public API ---- */

bool ext2_mount(const char *dev_name, const char *mount_point) {
    if (ext2.mounted) {
        kprintf("[EXT2] Already mounted\n");
        return false;
    }

    strncpy(ext2.dev_name, dev_name, sizeof(ext2.dev_name) - 1);
    ext2.dev_name[sizeof(ext2.dev_name) - 1] = '\0';

    if (!ext2_read_superblock()) return false;
    if (!ext2_read_bgd()) return false;

    ext2.mounted = true;
    kprintf("[EXT2] Mounted %s at %s (block_size=%u, inodes=%u)\n",
            dev_name, mount_point, ext2.block_size, ext2.sb.s_inodes_count);
    return true;
}

int ext2_read_file(const char *path, void *buf, size_t max_size, size_t *out_size) {
    if (!ext2.mounted) return -1;

    uint32_t ino = resolve_path(path);
    if (ino == 0) {
        kprintf("[EXT2] File not found: %s\n", path);
        return -1;
    }

    struct ext2_inode inode;
    if (!read_inode(ino, &inode)) return -1;

    if ((inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
        kprintf("[EXT2] Is a directory: %s\n", path);
        return -1;
    }

    uint32_t file_size = inode.i_size;
    if (out_size) *out_size = file_size;
    if (file_size > max_size) file_size = (uint32_t)max_size;

    return read_inode_data(&inode, 0, file_size, buf);
}

int ext2_list_dir(const char *path) {
    if (!ext2.mounted) return -1;

    struct ext2_dir_entry *entries = NULL;
    int count = 0;
    if (ext2_list_dir_internal(path, &entries, &count) != 0)
        return -1;

    kprintf("[EXT2] Directory %s (%d entries):\n", path, count);
    for (int i = 0; i < count; i++) {
        const char *type = (entries[i].file_type == EXT2_FT_DIR) ? "DIR " : "FILE";
        kprintf("  [%s] ino=%u %s\n", type, entries[i].inode_no, entries[i].name);
    }

    kfree(entries);
    return count;
}

bool ext2_exists(const char *path) {
    if (!ext2.mounted) return false;
    return resolve_path(path) != 0;
}

/* Helper to get all dir entries (used by VFS integration) */
int ext2_get_dir_entries(const char *path, struct ext2_dir_entry **out) {
    int count = 0;
    if (ext2_list_dir_internal(path, out, &count) != 0) return -1;
    return count;
}
