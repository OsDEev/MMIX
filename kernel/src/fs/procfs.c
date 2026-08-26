/*
 * /proc — dynamic process information filesystem.
 *
 * Files are generated on read; nothing is cached, so fork()/execve()
 * cannot leave stale references behind.
 *
 *   /proc/meminfo          RAM totals
 *   /proc/uptime           seconds since boot
 *   /proc/stat             one line per task (ps reads this)
 *   /proc/<pid>/status     per-process summary
 *   /proc/<pid>/cmdline    the exec path
 */
#include <kheap.h>
#include <stdarg.h>
#include <libk.h>
#include <pmm.h>
#include <sched.h>
#include <string.h>
#include <vfs.h>

/* node->data encodes (pid << 8) | kind */
#define PF_MEMINFO 1
#define PF_UPTIME  2
#define PF_STAT    3
#define PF_STATUS  4
#define PF_CMDLINE 5

#define PROCFS_POOL 32
static vfs_node_t pool[PROCFS_POOL];
static int pool_used = 0;

static vfs_node_t *make_node(vfs_node_t *dir, const char *name,
                             uint32_t payload) {
    if (pool_used >= PROCFS_POOL) return NULL;
    vfs_node_t *n = &pool[pool_used++];
    memset(n, 0, sizeof(*n));
    strncpy(n->name, name, VFS_MAX_NAME - 1);
    n->type = VFS_PROCFS;
    n->data = (uint8_t *)(uintptr_t)payload;
    n->parent = dir;
    n->next_sibling = dir->children;
    dir->children = n;
    return n;
}

void procfs_mount(vfs_node_t *root_node) {
    make_node(root_node, "meminfo", PF_MEMINFO);
    make_node(root_node, "uptime", PF_UPTIME);
    make_node(root_node, "stat", PF_STAT);
}

/* Resolve one component under a procfs node. Returns 0 on success. */
int procfs_resolve(vfs_node_t *dir, const char *component, vfs_node_t **out) {
    uint32_t base = (uint32_t)(uintptr_t)dir->data;

    /* Second level: <pid>/cmdline after <pid>/status */
    uint32_t dir_kind = base & 0xFF;
    uint32_t dir_pid = base >> 8;
    if (dir_kind == PF_STATUS || dir_kind == PF_CMDLINE) {
        if (strcmp(component, "cmdline") == 0) {
            /* reuse the pool slot: same node becomes the cmdline file */
            strncpy(dir->name, "cmdline", VFS_MAX_NAME - 1);
            dir->data = (uint8_t *)(uintptr_t)((dir_pid << 8) | PF_CMDLINE);
            *out = dir;
            return 0;
        }
        if (strcmp(component, "status") == 0) {
            *out = dir;
            return 0;
        }
        return -1;
    }

    /* First level under /proc */
    if (strcmp(component, "meminfo") == 0 ||
        strcmp(component, "uptime") == 0 ||
        strcmp(component, "stat") == 0) {
        for (vfs_node_t *c = dir->children; c; c = c->next_sibling) {
            if (strcmp(c->name, component) == 0) {
                *out = c;
                return 0;
            }
        }
        return -1;
    }

    /* Numeric component: <pid> -> status file */
    uint32_t pid = 0;
    for (const char *p = component; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        pid = pid * 10 + (uint32_t)(*p - '0');
    }
    if (pid == 0 || sched_find_pid((int)pid) == NULL) return -1;

    *out = make_node(dir, "status", (pid << 8) | PF_STATUS);
    return *out ? 0 : -1;
}

/* --- content generators --------------------------------------------------- */

static size_t emit(char *dst, size_t cap, const char *fmt, ...)
{
    /* Tiny formatter: %s %d %u only, appends into dst at `off` (clipped). */
    va_list ap;
    va_start(ap, fmt);
    char tmp[256];
    size_t t = 0;

    for (const char *p = fmt; *p && t < sizeof(tmp) - 1; p++) {
        if (*p != '%') { tmp[t++] = *p; continue; }
        p++;
        if (*p == 's') {
            const char *s = va_arg(ap, const char *);
            while (*s && t < sizeof(tmp) - 1) tmp[t++] = *s++;
        } else if (*p == 'c') {
            int v = va_arg(ap, int);
            tmp[t++] = (char)v;
        } else if (*p == 'd' || *p == 'u') {
            long v = va_arg(ap, long);
            char num[24];
            int i = 0;
            if (v < 0) { tmp[t++] = '-'; v = -v; }
            do { num[i++] = (char)('0' + v % 10); v /= 10; } while (v && i < 24);
            while (i && t < sizeof(tmp) - 1) tmp[t++] = num[--i];
        }
    }
    tmp[t] = '\0';
    va_end(ap);

    size_t n = strlen(tmp);
    if (n > cap) n = cap;
    memcpy(dst, tmp, n);
    return n;
}

int procfs_read(vfs_node_t *node, void *buf, size_t size, size_t offset) {
    uint32_t payload = (uint32_t)(uintptr_t)node->data;
    uint32_t kind = payload & 0xFF;
    uint32_t pid = payload >> 8;

    char content[1024];
    size_t len = 0;

    if (kind == PF_MEMINFO) {
        len = emit(content, sizeof(content) - 1,
                   "Total:  %u kB\nFree:    %u kB\n",
                   (long)(pmm_get_usable_bytes() / 1024),
                   (long)(pmm_get_free_pages() * PAGE_SIZE / 1024));
    } else if (kind == PF_UPTIME) {
        len = emit(content, sizeof(content) - 1, "%u.%u\n",
                   (long)(g_uptime_ticks / 50), (long)((g_uptime_ticks % 50) * 2));
    } else if (kind == PF_STAT) {
        for (int i = 0; i < MAX_TASKS; i++) {
            task_t *t = sched_task_at(i);
            if (t == NULL || t->state == TASK_FREE) continue;
            char state = t->zombie ? 'Z' :
                         (t->state == TASK_RUNNING ? 'R' :
                          (t->state == TASK_READY ? 'R' : 'S'));
            len += emit(content + len, sizeof(content) - 1 - len,
                        "%d %d %d %c %s\n",
                        (long)t->pid, (long)t->parent_pid,
                        (long)t->pgid, state, t->name);
            if (len > sizeof(content) - 128) break;
        }
    } else if (kind == PF_STATUS || kind == PF_CMDLINE) {
        task_t *t = sched_find_pid((int)pid);
        if (t == NULL) return -1;
        if (kind == PF_CMDLINE) {
            len = emit(content, sizeof(content) - 1, "%s\n", t->name);
        } else {
            char state = t->zombie ? 'Z' :
                         (t->state == TASK_RUNNING ? 'R' :
                          (t->state == TASK_READY ? 'R' : 'S'));
            len = emit(content, sizeof(content) - 1,
                       "Name:   %s\nPid:    %d\nPPid:   %d\nPgId:   %d\nState:  %c\n",
                       t->name, (long)t->pid, (long)t->parent_pid,
                       (long)t->pgid, state);
        }
    } else {
        return -1;
    }

    if (offset >= len) return 0;
    size_t n = len - offset;
    if (n > size) n = size;
    memcpy(buf, content + offset, n);
    return (int)n;
}
