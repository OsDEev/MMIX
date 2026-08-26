#include <libk.h>
#include <limine.h>
#include <pmm.h>
#include <string.h>

extern volatile struct limine_memmap_request memmap_request;
extern volatile struct limine_hhdm_request hhdm_request;

static uint8_t *bitmap = NULL;
static uint64_t usable_bytes = 0;
static size_t bitmap_size = 0;
static uint64_t total_pages = 0;
static uint64_t free_pages = 0;
static uint64_t highest_page = 0;

static inline void bitmap_set(uint64_t page) {
    bitmap[page / 8] |= (1 << (page % 8));
}

static inline void bitmap_clear(uint64_t page) {
    bitmap[page / 8] &= ~(1 << (page % 8));
}

static inline bool bitmap_test(uint64_t page) {
    return bitmap[page / 8] & (1 << (page % 8));
}

void pmm_init(void) {
    if (memmap_request.response == NULL) {
        kprintf("[PMM] PANIC: no memory map\n");
        return;
    }

    /* Find the highest usable address */
    uint64_t top = 0;
    for (size_t i = 0; i < memmap_request.response->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap_request.response->entries[i];
        uint64_t entry_top = entry->base + entry->length;
        if (entry_top > top) {
            top = entry_top;
        }
    }

    total_pages = top / PAGE_SIZE;
    highest_page = total_pages;
    bitmap_size = (total_pages + 7) / 8;

    /* Find a usable region big enough for the bitmap */
    uint64_t hhdm = hhdm_request.response->offset;
    for (size_t i = 0; i < memmap_request.response->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap_request.response->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE && entry->length >= bitmap_size) {
            bitmap = (uint8_t *)(entry->base + hhdm);
            memset(bitmap, 0xFF, bitmap_size); /* Mark all as used */
            break;
        }
    }

    if (bitmap == NULL) {
        kprintf("[PMM] PANIC: no space for bitmap\n");
        return;
    }

    /* Free usable regions */
    free_pages = 0;
    usable_bytes = 0;
    for (size_t i = 0; i < memmap_request.response->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap_request.response->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE) continue;

        usable_bytes += entry->length;

        uint64_t start_page = entry->base / PAGE_SIZE;
        uint64_t page_count = entry->length / PAGE_SIZE;

        for (uint64_t p = start_page; p < start_page + page_count; p++) {
            if (p < total_pages) {
                bitmap_clear(p);
                free_pages++;
            }
        }
    }

    /* Reserve the pages the bitmap itself occupies. */
    uint64_t bm_phys = (uint64_t)bitmap - hhdm_request.response->offset;
    uint64_t bm_page = bm_phys / PAGE_SIZE;
    uint64_t bm_count = (bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = 0; i < bm_count; i++) {
        if (!bitmap_test(bm_page + i)) {
            bitmap_set(bm_page + i);
            free_pages--;
        }
    }

    highest_page = bm_page; /* keep the symbol meaningful */

    kprintf("[PMM] Initialized: %u MiB total, %u MiB free, %u pages\n",
            usable_bytes / (1024 * 1024),
            (free_pages * PAGE_SIZE) / (1024 * 1024),
            free_pages);
}

void *pmm_alloc(size_t pages) {
    if (pages == 0) return NULL;

    size_t consecutive = 0;
    uint64_t start = 0;

    for (uint64_t i = 0; i < total_pages; i++) {
        if (!bitmap_test(i)) {
            if (consecutive == 0) start = i;
            consecutive++;
            if (consecutive == pages) {
                for (size_t j = 0; j < pages; j++) {
                    bitmap_set(start + j);
                }
                free_pages -= pages;
                return (void *)(start * PAGE_SIZE + hhdm_request.response->offset);
            }
        } else {
            consecutive = 0;
        }
    }

    return NULL; /* Out of memory */
}

void pmm_free(void *addr, size_t pages) {
    uint64_t phys = (uint64_t)addr - hhdm_request.response->offset;
    uint64_t start_page = phys / PAGE_SIZE;

    for (size_t i = 0; i < pages; i++) {
        if (!bitmap_test(start_page + i)) {
            kprintf("[PMM] WARNING: double free at page %u\n", start_page + i);
            return;
        }
        bitmap_clear(start_page + i);
        free_pages++;
    }
}

uint64_t pmm_get_usable_bytes(void) { return usable_bytes; }
uint64_t pmm_get_free_pages(void) { return free_pages; }
uint64_t pmm_get_total_pages(void) { return total_pages; }

void *pmm_alloc_dma(size_t pages) {
    if (pages == 0) return NULL;
    uint64_t max_page = (4ULL * 1024 * 1024 * 1024) / PAGE_SIZE;
    if (max_page > total_pages) max_page = total_pages;

    size_t consecutive = 0;
    uint64_t start = 0;
    for (uint64_t i = 0; i < max_page; i++) {
        if (!bitmap_test(i)) {
            if (consecutive == 0) start = i;
            consecutive++;
            if (consecutive == pages) {
                for (size_t j = 0; j < pages; j++)
                    bitmap_set(start + j);
                free_pages -= pages;
                return (void *)(start * PAGE_SIZE + hhdm_request.response->offset);
            }
        } else {
            consecutive = 0;
        }
    }
    return NULL;
}
