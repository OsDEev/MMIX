#ifndef MYUNIX_KBD_H
#define MYUNIX_KBD_H

#include <stdbool.h>

/* Called from the IRQ1 path. */
void kbd_interrupt(void);

/* Blocking character read (ASCII). Returns -1 when interrupted. */
int kbd_getchar(void);
bool kbd_poll(char *out);

#endif /* MYUNIX_KBD_H */
