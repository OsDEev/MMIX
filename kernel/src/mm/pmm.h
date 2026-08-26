#ifndef MYUNIX_PMM_H
#define MYUNIX_PMM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PAGE_SIZE 4096ULL

void pmm_init(void);
void *pmm_alloc(size_t pages);
void pmm_free(void *addr, size_t pages);
uint64_t pmm_get_free_pages(void);
uint64_t pmm_get_usable_bytes(void);
uint64_t pmm_get_total_pages(void);

#endif /* MYUNIX_PMM_H */
