#include <kheap.h>
#include <libk.h>
#include <pmm.h>
#include <string.h>

#define HEAP_INITIAL_PAGES 16
#define HEAP_MAGIC 0xDEADBEEF
#define HEAP_MIN_SPLIT 64

struct heap_block {
    uint32_t magic;
    size_t   size;
    bool     free;
    struct heap_block *next;
    struct heap_block *prev;
} __attribute__((packed));

static struct heap_block *heap_start = NULL;
static void *heap_end = NULL;

static bool heap_expand(size_t min_size) {
    size_t pages = (min_size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages < 4) pages = 4;

    void *new_pages = pmm_alloc(pages);
    if (new_pages == NULL) return false;

    struct heap_block *block = (struct heap_block *)new_pages;
    block->magic = HEAP_MAGIC;
    block->size  = pages * PAGE_SIZE - sizeof(struct heap_block);
    block->free  = true;
    block->next  = NULL;
    block->prev  = NULL;

    if (heap_start == NULL) {
        heap_start = block;
    } else {
        /* Find last block */
        struct heap_block *last = heap_start;
        while (last->next != NULL) last = last->next;
        last->next = block;
        block->prev = last;
    }

    heap_end = (uint8_t *)new_pages + pages * PAGE_SIZE;
    return true;
}

static void heap_coalesce(void) {
    struct heap_block *block = heap_start;
    while (block != NULL) {
        if (block->free && block->next != NULL && block->next->free) {
            block->size += sizeof(struct heap_block) + block->next->size;
            block->next = block->next->next;
            if (block->next != NULL) {
                block->next->prev = block;
            }
        }
        block = block->next;
    }
}

void heap_init(void) {
    if (!heap_expand(HEAP_INITIAL_PAGES * PAGE_SIZE)) {
        kprintf("[HEAP] PANIC: cannot allocate initial heap\n");
        return;
    }
    kprintf("[HEAP] Initialized at %p\n", (void *)heap_start);
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;

    /* Align to 16 bytes */
    size = (size + 15) & ~(size_t)15;

    struct heap_block *block = heap_start;
    while (block != NULL) {
        if (block->free && block->size >= size) {
            /* Split if worth it */
            if (block->size > size + sizeof(struct heap_block) + HEAP_MIN_SPLIT) {
                struct heap_block *new_block =
                    (struct heap_block *)((uint8_t *)block + sizeof(struct heap_block) + size);
                new_block->magic = HEAP_MAGIC;
                new_block->size  = block->size - size - sizeof(struct heap_block);
                new_block->free  = true;
                new_block->next  = block->next;
                new_block->prev  = block;
                if (block->next != NULL) {
                    block->next->prev = new_block;
                }
                block->next = new_block;
                block->size = size;
            }
            block->free = false;
            return (uint8_t *)block + sizeof(struct heap_block);
        }
        block = block->next;
    }

    /* Expand heap and retry */
    if (!heap_expand(size)) return NULL;
    return kmalloc(size);
}

void *kcalloc(size_t count, size_t size) {
    size_t total = count * size;
    void *ptr = kmalloc(total);
    if (ptr != NULL) memset(ptr, 0, total);
    return ptr;
}

void *krealloc(void *ptr, size_t size) {
    if (ptr == NULL) return kmalloc(size);
    if (size == 0) { kfree(ptr); return NULL; }

    struct heap_block *block =
        (struct heap_block *)((uint8_t *)ptr - sizeof(struct heap_block));
    if (block->magic != HEAP_MAGIC) {
        kprintf("[HEAP] CORRUPTION detected in krealloc!\n");
        return NULL;
    }

    if (block->size >= size) return ptr;

    void *new_ptr = kmalloc(size);
    if (new_ptr == NULL) return NULL;
    memcpy(new_ptr, ptr, block->size);
    kfree(ptr);
    return new_ptr;
}

void kfree(void *ptr) {
    if (ptr == NULL) return;

    struct heap_block *block =
        (struct heap_block *)((uint8_t *)ptr - sizeof(struct heap_block));
    if (block->magic != HEAP_MAGIC) {
        kprintf("[HEAP] CORRUPTION: invalid magic in kfree at %p\n", ptr);
        return;
    }

    block->free = true;
    heap_coalesce();
}
