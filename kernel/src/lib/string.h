/*
 * Freestanding string/memory routines for the kernel.
 * Resolves <string.h> inside the kernel build instead of the host libc.
 */
#ifndef MYUNIX_STRING_H
#define MYUNIX_STRING_H

#include <stddef.h>

void *memcpy(void *restrict dest, const void *restrict src, size_t n);
void *memset(void *s, int c, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int   memcmp(const void *s1, const void *s2, size_t n);

char *strncpy(char *restrict dest, const char *restrict src, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);

#endif /* MYUNIX_STRING_H */
