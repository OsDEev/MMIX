#include <kheap.h>
#include <libk.h>
#include <limine.h>
#include <pmap.h>
#include <pmm.h>
#include <sched.h>
#include <string.h>

extern volatile struct limine_hhdm_request hhdm_request;
extern volatile struct limine_memmap_request memmap_request;

uint64_t kernel_pml4 = 0;

/* Stack growth parameters used by the fault handler. */
#define USER_STACK_TOP_VA   0x70000000ULL
#define USER_STACK_MAX_SIZE (8ULL * 1024 * 1024)

static uint32_t *refs = NULL; /* per-frame reference counts */
static uint64_t refs_pages = 0;

static inline uint64_t hhdm_of(uint64_t phys) {
    return phys + hhdm_request.response->offset;
}

static inline uint64_t read_cr3(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}

void frame_refs_init(uint64_t total_pages_hint) {
    (void)total_pages_hint;

    /* Size the table by the highest USABLE frame, not by the top of the
     * physical address space (reserved MMIO holes can reach 1 TiB). */
    uint64_t max_page = 0;
    for (size_t i = 0; i < memmap_request.response->entry_count; i++) {
        struct limine_memmap_entry *e = memmap_request.response->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        uint64_t top = (e->base + e->length) / PAGE_SIZE;
        if (top > max_page) max_page = top;
    }

    size_t bytes = (size_t)max_page * sizeof(uint32_t);
    size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    void *mem = pmm_alloc(pages);
    if (mem == NULL) {
        kprintf("[PMAP] PANIC: cannot allocate frame refcounts\n");
        return;
    }
    memset(mem, 0, pages * PAGE_SIZE);

    refs = (uint32_t *)mem;
    refs_pages = max_page;
}

static inline bool ref_valid(uint64_t pa) {
    return refs != NULL && (pa / PAGE_SIZE) < refs_pages;
}

void frame_ref_inc(uint64_t phys) {
    if (ref_valid(phys)) refs[phys / PAGE_SIZE]++;
}

bool frame_ref_dec(uint64_t phys) {
    if (!ref_valid(phys)) return false;
    if (refs[phys / PAGE_SIZE] > 0) refs[phys / PAGE_SIZE]--;
    return refs[phys / PAGE_SIZE] == 0;
}

uint64_t pmap_create(void) {
    void *page = pmm_alloc(1);
    if (page == NULL) {
        kprintf("[PMAP] PANIC: cannot allocate PML4\n");
        return 0;
    }

    /* pmm_alloc hands out HHDM pointers; the PML4 is tracked by its phys. */
    uint64_t pml4_phys = (uint64_t)page - hhdm_request.response->offset;
    memset(page, 0, PAGE_SIZE);

    /* Clone every upper-half entry (kernel image + HHDM + LAPIC). */
    uint64_t *old = (uint64_t *)hhdm_of(read_cr3() & PTE_ADDR_MASK);
    uint64_t *pml4 = (uint64_t *)page;
    for (size_t i = PAGE_TABLE_ENTRIES / 2; i < PAGE_TABLE_ENTRIES; i++) {
        pml4[i] = old[i];
    }

    return pml4_phys;
}

/*
 * Allocate an intermediate table. Note: PMM pages come back as HHDM
 * pointers; convert back to physical for the PTE.
 */
static uint64_t table_alloc(void) {
    void *page = pmm_alloc(1);
    if (page == NULL) {
        kprintf("[PMAP] PANIC: out of memory for page tables\n");
        return 0;
    }
    memset(page, 0, PAGE_SIZE);
    return (uint64_t)page - hhdm_request.response->offset;
}

static inline uint64_t idx(uint64_t va, int level) {
    return (va >> (12 + level * 9)) & 0x1FF;
}

void pmap_map(uint64_t pml4_phys, uint64_t va, uint64_t pa, uint64_t flags) {
    uint64_t *table = (uint64_t *)hhdm_of(pml4_phys);

    for (int level = 3; level > 0; level--) {
        uint64_t entry = table[idx(va, level)];

        if ((entry & PTE_PRESENT) == 0) {
            uint64_t next_phys = table_alloc();
            if (next_phys == 0) {
                kprintf("[MAP] alloc fail va=%x L%d\n", va, level);
                return;
            }
            entry = next_phys | PTE_PRESENT | PTE_WRITE | flags;
            table[idx(va, level)] = entry;
        }

        table = (uint64_t *)hhdm_of(entry & PTE_ADDR_MASK);
    }

    table[idx(va, 0)] = (pa & PTE_ADDR_MASK) | PTE_PRESENT | flags;
}

uint64_t pmap_resolve(uint64_t pml4_phys, uint64_t va) {
    uint64_t *leaf = pmap_pte(pml4_phys, va);
    if (leaf == NULL || (*leaf & PTE_PRESENT) == 0) return 0;
    return (*leaf & PTE_ADDR_MASK) | (va & 0xFFFULL);
}

uint64_t *pmap_pte(uint64_t pml4_phys, uint64_t va) {
    uint64_t *table = (uint64_t *)hhdm_of(pml4_phys);

    for (int level = 3; level > 0; level--) {
        uint64_t entry = table[idx(va, level)];
        if ((entry & PTE_PRESENT) == 0) return NULL;
        table = (uint64_t *)hhdm_of(entry & PTE_ADDR_MASK);
    }

    return &table[idx(va, 0)];
}

void pmap_unmap(uint64_t pml4_phys, uint64_t va) {
    uint64_t *pte = pmap_pte(pml4_phys, va);
    if (pte != NULL) {
        *pte = 0;
        pmap_invlpg(va);
    }
}

/* --- address space duplication ----------------------------------------- */

uint64_t pmap_fork(uint64_t parent) {
    uint64_t child = pmap_create();
    if (child == 0) return 0;

    uint64_t *ppml4 = (uint64_t *)hhdm_of(parent);

    for (uint64_t p4 = 0; p4 < PAGE_TABLE_ENTRIES / 2; p4++) {
        uint64_t e4 = ppml4[p4];
        if ((e4 & PTE_PRESENT) == 0) continue;

        uint64_t *pdpt = (uint64_t *)hhdm_of(e4 & PTE_ADDR_MASK);
        for (uint64_t p3 = 0; p3 < PAGE_TABLE_ENTRIES; p3++) {
            uint64_t e3 = pdpt[p3];
            if ((e3 & PTE_PRESENT) == 0) continue;

            uint64_t *pd = (uint64_t *)hhdm_of(e3 & PTE_ADDR_MASK);
            for (uint64_t p2 = 0; p2 < PAGE_TABLE_ENTRIES; p2++) {
                uint64_t e2 = pd[p2];
                if ((e2 & PTE_PRESENT) == 0) continue;

                uint64_t *pt = (uint64_t *)hhdm_of(e2 & PTE_ADDR_MASK);
                for (uint64_t p1 = 0; p1 < PAGE_TABLE_ENTRIES; p1++) {
                    uint64_t pte = pt[p1];
                    if ((pte & PTE_PRESENT) == 0) continue;

                    uint64_t pa = pte & PTE_ADDR_MASK;
                    uint64_t flags = pte & ~(PTE_ADDR_MASK | PTE_WRITE | PTE_COW);
                    uint64_t va =
                        (p4 << 39) | (p3 << 30) | (p2 << 21) | (p1 << 12);

                    /*
                     * Eager private copy.
                     *
                     * NOTE: full copy-on-write sharing exists in this tree
                     * (PTE_COW soft bit, frame refcounts and the #PF break
                     * path in pmap_handle_fault), but enabling it for fork
                     * currently races with timer preemption; until that is
                     * root-caused, fork pays for a full copy up front.
                     */
                    void *frame = pmm_alloc(1);
                    if (frame == NULL) return 0;
                    memcpy(frame, (void *)hhdm_of(pa), PAGE_SIZE);
                    uint64_t new_pa = (uint64_t)frame - hhdm_request.response->offset;
                    frame_ref_inc(new_pa);
                    pmap_map(child, va, new_pa, flags | PTE_WRITE);
                }
            }
        }
    }

    return child;
}

void pmap_destroy(uint64_t pml4_phys) {
    uint64_t *pml4 = (uint64_t *)hhdm_of(pml4_phys);

    for (uint64_t p4 = 0; p4 < PAGE_TABLE_ENTRIES / 2; p4++) {
        uint64_t e4 = pml4[p4];
        if ((e4 & PTE_PRESENT) == 0) continue;

        uint64_t *pdpt = (uint64_t *)hhdm_of(e4 & PTE_ADDR_MASK);
        for (uint64_t p3 = 0; p3 < PAGE_TABLE_ENTRIES; p3++) {
            uint64_t e3 = pdpt[p3];
            if ((e3 & PTE_PRESENT) == 0) continue;

            uint64_t *pd = (uint64_t *)hhdm_of(e3 & PTE_ADDR_MASK);
            for (uint64_t p2 = 0; p2 < PAGE_TABLE_ENTRIES; p2++) {
                uint64_t e2 = pd[p2];
                if ((e2 & PTE_PRESENT) == 0) continue;

                uint64_t *pt = (uint64_t *)hhdm_of(e2 & PTE_ADDR_MASK);
                for (uint64_t p1 = 0; p1 < PAGE_TABLE_ENTRIES; p1++) {
                    uint64_t pte = pt[p1];
                    if ((pte & PTE_PRESENT) == 0) continue;

                    uint64_t pa = pte & PTE_ADDR_MASK;
                    if (frame_ref_dec(pa)) {
                        pmm_free((void *)hhdm_of(pa), 1);
                    }
                }
                pmm_free((void *)hhdm_of(e2 & PTE_ADDR_MASK), 1);
            }
            pmm_free((void *)hhdm_of(e3 & PTE_ADDR_MASK), 1);
        }
        pmm_free((void *)hhdm_of(e4 & PTE_ADDR_MASK), 1);
    }

    pmm_free((void *)hhdm_of(pml4_phys), 1);
}

/* --- page fault resolution --------------------------------------------- */

bool pmap_handle_fault(uint64_t va, uint64_t err_code) {
    task_t *cur = sched_get_current();
    if (cur == NULL || cur->pml4_phys == 0) return false;

    const bool present = err_code & 1;
    const bool write = err_code & 2;
    const bool user = err_code & 4;
    (void)user;

    if (!present) {
        /* Automatic stack growth below the mapped region. */
        if (user && va < USER_STACK_TOP_VA &&
            va >= USER_STACK_TOP_VA - USER_STACK_MAX_SIZE) {
            uint64_t aligned = va & ~0xFFFULL;
            if (pmap_resolve(cur->pml4_phys, aligned) != 0) return false;

            void *frame = pmm_alloc(1);
            if (frame == NULL) return false;
            memset(frame, 0, PAGE_SIZE);
            pmap_map(cur->pml4_phys, aligned,
                     (uint64_t)frame - hhdm_request.response->offset,
                     PTE_USER | PTE_WRITE);
            return true;
        }
        return false;
    }

    /* CoW break */
    if (write) {
        uint64_t *pte = pmap_pte(cur->pml4_phys, va);
        if (pte == NULL || (*pte & PTE_COW) == 0) return false;

        uint64_t pa = *pte & PTE_ADDR_MASK;
        uint64_t flags = *pte & (PTE_USER | PTE_NOCACHE);

        if (ref_valid(pa) && refs[pa / PAGE_SIZE] > 1) {
            refs[pa / PAGE_SIZE]--;

            void *frame = pmm_alloc(1);
            if (frame == NULL) return false;
            uint64_t new_pa = (uint64_t)frame - hhdm_request.response->offset;
            memcpy((void *)hhdm_of(new_pa), (void *)hhdm_of(pa), PAGE_SIZE);
            refs[new_pa / PAGE_SIZE] = 1;

            *pte = new_pa | PTE_PRESENT | PTE_WRITE | flags;
        } else {
            /* Sole owner: just regain write access. */
            *pte |= PTE_WRITE;
            *pte &= ~PTE_COW;
        }

        pmap_invlpg(va);
        return true;
    }

    return false;
}
