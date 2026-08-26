#include <stdint.h>
#include <stddef.h>
#include <string.h>

void *memcpy(void *restrict dest, const void *restrict src, size_t n) {
    uint8_t *restrict d = (uint8_t *restrict)dest;
    const uint8_t *restrict s = (const uint8_t *restrict)src;

    if (n >= 8 && ((uintptr_t)d & 7) == 0 && ((uintptr_t)s & 7) == 0) {
        uint64_t *d64 = (uint64_t *)d;
        const uint64_t *s64 = (const uint64_t *)s;
        size_t words = n / 8;
        for (size_t i = 0; i < words; i++) {
            d64[i] = s64[i];
        }
        d += words * 8;
        s += words * 8;
        n &= 7;
    }

    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;

    if (n >= 8 && ((uintptr_t)p & 7) == 0) {
        uint64_t fill = (uint8_t)c;
        fill |= fill << 8;
        fill |= fill << 16;
        fill |= fill << 32;
        uint64_t *p64 = (uint64_t *)p;
        size_t words = n / 8;
        for (size_t i = 0; i < words; i++) {
            p64[i] = fill;
        }
        p += words * 8;
        n &= 7;
    }

    while (n--) {
        *p++ = (uint8_t)c;
    }
    return s;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    if (d == s || n == 0) {
        return dest;
    }

    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;

    while (n--) {
        if (*p1 != *p2) {
            return *p1 - *p2;
        }
        p1++;
        p2++;
    }
    return 0;
}

char *strncpy(char *restrict dest, const char *restrict src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : NULL;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    return (c == '\0') ? (char *)s : (char *)last;
}
