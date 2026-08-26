#ifndef MYUNIX_PMAP_H
#define MYUNIX_PMAP_H

#include <stdbool.h>
#include <stdint.h>

#define PTE_PRESENT 0x001ULL
#define PTE_WRITE   0x002ULL
#define PTE_USER    0x004ULL
#define PTE_NOCACHE 0x010ULL
#define PTE_COW     0x200ULL /* software bit (AVAIL#1): copy-on-write */
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

#define PAGE_TABLE_ENTRIES 512

/* Physical base of the shared kernel/user address space (see main.c). */
extern uint64_t kernel_pml4;

/* Create a fresh address space: upper half cloned from the current CR3. */
uint64_t pmap_create(void);

/* Map one 4 KiB page. Intermediate tables are allocated from the PMM. */
void pmap_map(uint64_t pml4_phys, uint64_t va, uint64_t pa, uint64_t flags);

/* Physical address of a mapped VA (0 if not present). */
uint64_t pmap_resolve(uint64_t pml4_phys, uint64_t va);

/* Pointer to the leaf PTE of a mapped page (NULL if not present). */
uint64_t *pmap_pte(uint64_t pml4_phys, uint64_t va);

/* Drop a mapping (leaf PTE cleared, TLB invalidated). */
void pmap_unmap(uint64_t pml4_phys, uint64_t va);

/*
 * Copy-on-write support:
 *   pmap_fork    - duplicate the user half of `parent` into a fresh space,
 *                  sharing frames read-only with PTE_COW markers.
 *   pmap_destroy - free every user-half table and frame of a space.
 *   pmap_handle_fault - try to resolve #PF as CoW break or stack growth;
 *                  returns true when handled.
 */
uint64_t pmap_fork(uint64_t parent);
void pmap_destroy(uint64_t pml4_phys);
bool pmap_handle_fault(uint64_t va, uint64_t err_code);

/* Frame reference counting backing CoW. */
void frame_refs_init(uint64_t total_pages);
void frame_ref_inc(uint64_t phys);
/* Returns true when this was the last reference. */
bool frame_ref_dec(uint64_t phys);

/* Switch to an address space (flushes TLB). */
static inline void pmap_load(uint64_t pml4_phys) {
    __asm__ volatile("mov %0, %%cr3" :: "r"(pml4_phys) : "memory");
}

static inline void pmap_invlpg(uint64_t va) {
    __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
}

#endif /* MYUNIX_PMAP_H */
