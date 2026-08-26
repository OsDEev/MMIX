#ifndef MYUNIX_PMM_H
#define MYUNIX_PMM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limine.h>

#define PAGE_SIZE 4096ULL

void pmm_init(void);
void *pmm_alloc(size_t pages);
void pmm_free(void *addr, size_t pages);
uint64_t pmm_get_free_pages(void);
uint64_t pmm_get_usable_bytes(void);
uint64_t pmm_get_total_pages(void);

/* DMA-safe: allocates physical pages with PA < 4 GiB. */
void *pmm_alloc_dma(size_t pages);

/* Virtual <-> physical address translation (kernel HHDM). */
static inline uint64_t virt_to_phys(const void *virt) {
    extern volatile struct limine_hhdm_request hhdm_request;
    return (uint64_t)virt - hhdm_request.response->offset;
}

static inline void *phys_to_virt(uint64_t phys) {
    extern volatile struct limine_hhdm_request hhdm_request;
    return (void *)(phys + hhdm_request.response->offset);
}

#endif /* MYUNIX_PMM_H */
