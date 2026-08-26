#ifndef MYUNIX_ELF_H
#define MYUNIX_ELF_H

#include <stdint.h>

#define EXEC_MAX_ARGS    32
#define EXEC_MAX_ARG_LEN 256

struct exec_image {
    uint64_t pml4_phys;
    uint64_t entry;
    uint64_t user_rsp;
    uint64_t brk_base;
};

/*
 * Load an executable from the VFS into a brand-new address space and
 * prepare its initial stack (argc/argv per SysV ABI).
 *
 * argv must point to kernel memory (NUL-terminated strings). Returns 0
 * on success.
 */
int exec_build_image(const char *path, char *const argv[],
                     struct exec_image *out);

#endif /* MYUNIX_ELF_H */
