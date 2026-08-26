#ifndef MYUNIX_TTY_H
#define MYUNIX_TTY_H

#include <stddef.h>
#include <stdint.h>

struct limine_framebuffer;

void tty_init(struct limine_framebuffer *fb);
void tty_putc(char c);
void tty_write(const char *buf, size_t n);
void tty_clear(void);
void tty_get_mode(uint32_t *w, uint32_t *h, uint32_t *bpp);

#endif /* MYUNIX_TTY_H */
