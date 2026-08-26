#ifndef MYUNIX_LIBK_H
#define MYUNIX_LIBK_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Early console (COM1). Used before/without a proper tty driver. */
void serial_init(void);
void kprint(const char *str);
void kprintf(const char *fmt, ...);
void vsnprintf_mini(char *out, size_t cap, const char *fmt, va_list ap);

/* Minimal libc subset shared by the kernel. */
size_t strlen(const char *s);
int strcmp(const char *s1, const char *s2);

#endif /* MYUNIX_LIBK_H */
