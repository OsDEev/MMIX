#include <kheap.h>
#include <libk.h>
#include <limine.h>
#include <pmap.h>
#include <pmm.h>
#include <string.h>
#include <vfs.h>

#include "elf.h"

typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) Elf64_Phdr;

#define ELF_MAGIC_VAL 0x464C457F
#define ELF_EXEC      2
#define ELF_X86_64    62
#define PT_LOAD       1

#define USER_STACK_TOP     0x70000000ULL
#define USER_STACK_PAGES   16
#define EXEC_MAX_ARGS      32
#define EXEC_MAX_ARG_LEN   256

extern volatile struct limine_hhdm_request hhdm_request;

static inline uint64_t hhdm_of(void *ptr) {
    return (uint64_t)ptr - hhdm_request.response->offset;
}

/* Page-safe write into a user VA: never crosses a frame linearly. */
static bool copy_to_va(uint64_t pml4, uint64_t va, const void *src, size_t len) {
    size_t done = 0;
    while (done < len) {
        uint64_t cur = va + done;
        uint64_t pa = pmap_resolve(pml4, cur & ~0xFFFULL);
        if (pa == 0) return false;
        size_t chunk = len - done;
        if (chunk > PAGE_SIZE - (cur & 0xFFFULL)) {
            chunk = PAGE_SIZE - (cur & 0xFFFULL);
        }
        memcpy((void *)(pa + hhdm_request.response->offset + (cur & 0xFFFULL)),
               (const uint8_t *)src + done, chunk);
        done += chunk;
    }
    return true;
}

static bool map_zeroed_range(uint64_t pml4, uint64_t va, size_t bytes) {
    size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    for (size_t i = 0; i < pages; i++) {
        void *frame = pmm_alloc(1);
        if (frame == NULL) return false;
        memset(frame, 0, PAGE_SIZE);
        pmap_map(pml4, va + i * PAGE_SIZE,
                 hhdm_of(frame), PTE_USER | PTE_WRITE);
    }
    return true;
}

/* Copy argv onto the new user stack, SysV style:
 *   [argc][argv0..argvN][NULL][envp=NULL]  (rsp points at argc)
 */
static uint64_t build_user_stack(uint64_t pml4, char *const argv[]) {
    uint64_t top = USER_STACK_TOP;

    if (!map_zeroed_range(pml4, top - USER_STACK_PAGES * PAGE_SIZE,
                          USER_STACK_PAGES * PAGE_SIZE)) {
        return 0;
    }

    int argc = 0;
    while (argv != NULL && argv[argc] != NULL && argc < EXEC_MAX_ARGS) argc++;

    /* Copy strings downward from the very top. */
    uint64_t sp = top;
    uint64_t str_addrs[EXEC_MAX_ARGS];

    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;
        if (len > EXEC_MAX_ARG_LEN) len = EXEC_MAX_ARG_LEN;
        sp -= len;

        if (!copy_to_va(pml4, sp, argv[i], len)) return 0;
        str_addrs[i] = sp;
    }

    /* Align for the pointer vector. */
    sp &= ~0xFULL;

    /* argv[0..n], NULL, envp NULL */
    uint64_t vec[EXEC_MAX_ARGS + 2];
    for (int i = 0; i < argc; i++) vec[i] = str_addrs[i];
    vec[argc] = 0;
    vec[argc + 1] = 0;

    size_t vec_bytes = (size_t)(argc + 2) * sizeof(uint64_t);
    sp -= vec_bytes;
    sp &= ~0xFULL;

    if (!copy_to_va(pml4, sp, vec, vec_bytes)) return 0;

    /* argc */
    sp -= sizeof(uint64_t);
    uint64_t one = (uint64_t)argc;
    if (!copy_to_va(pml4, sp, &one, sizeof(one))) return 0;

    return sp;
}

int exec_build_image(const char *path, char *const argv[],
                     struct exec_image *out) {
    vfs_node_t *node = vfs_resolve(path);
    if (node == NULL || node->type != VFS_FILE) {
        kprintf("[ELF] File not found: %s\n", path);
        return -1;
    }
    if (node->size < sizeof(Elf64_Ehdr)) {
        kprintf("[ELF] File too small: %s\n", path);
        return -1;
    }

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)node->data;

    if (*(uint32_t *)ehdr->e_ident != ELF_MAGIC_VAL ||
        ehdr->e_type != ELF_EXEC || ehdr->e_machine != ELF_X86_64) {
        kprintf("[ELF] Not an x86_64 executable: %s\n", path);
        return -1;
    }

    uint64_t pml4 = pmap_create();
    if (pml4 == 0) return -1;

    Elf64_Phdr *phdrs = (Elf64_Phdr *)(node->data + ehdr->e_phoff);
    uint64_t image_end = 0;
    bool any_load = false;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        any_load = true;

        uint64_t seg_end = phdrs[i].p_vaddr + phdrs[i].p_memsz;
        if (seg_end > image_end) image_end = seg_end;

        if (!map_zeroed_range(pml4, phdrs[i].p_vaddr, phdrs[i].p_memsz)) {
            kprintf("[ELF] OOM mapping %s\n", path);
            pmap_destroy(pml4);
            return -1;
        }
    }

    if (!any_load) {
        kprintf("[ELF] No PT_LOAD segments in %s\n", path);
        pmap_destroy(pml4);
        return -1;
    }

    /* Fill segments. Copy page-by-page through the mapping: the
     * segment's physical frames are NOT contiguous -- page tables
     * allocated between them would be smashed by one linear memcpy. */
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD || phdrs[i].p_filesz == 0) continue;

        size_t done = 0;
        while (done < phdrs[i].p_filesz) {
            uint64_t va = phdrs[i].p_vaddr + done;
            uint64_t pa = pmap_resolve(pml4, va);
            size_t chunk = phdrs[i].p_filesz - done;
            if (chunk > PAGE_SIZE - (va & 0xFFFULL)) {
                chunk = PAGE_SIZE - (va & 0xFFFULL);
            }
            memcpy((void *)(pa + hhdm_request.response->offset + (va & 0xFFFULL)),
                   node->data + phdrs[i].p_offset + done, chunk);
            done += chunk;
        }
    }

    uint64_t stack_rsp = build_user_stack(pml4, argv);
    if (stack_rsp == 0) {
        kprintf("[ELF] Cannot build stack for %s\n", path);
        pmap_destroy(pml4);
        return -1;
    }

    out->pml4_phys = pml4;
    out->entry = ehdr->e_entry;
    out->user_rsp = stack_rsp;
    out->brk_base = (image_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    return 0;
}
