#ifndef MYUNIX_KHEAP_H
#define MYUNIX_KHEAP_H

#include <stddef.h>

void heap_init(void);
void *kmalloc(size_t size);
void *kcalloc(size_t count, size_t size);
void *krealloc(void *ptr, size_t size);
void kfree(void *ptr);

#endif /* MYUNIX_KHEAP_H */
