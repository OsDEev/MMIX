#ifndef MYUNIX_PANIC_H
#define MYUNIX_PANIC_H

/*
 * MMIX kernel panic: full-screen fault screen (BSOD analog).
 * Disables interrupts, paints the framebuffer, prints context and
 * halts forever. Never returns.
 */
void panic(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

#endif /* MYUNIX_PANIC_H */
