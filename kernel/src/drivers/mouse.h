#ifndef MYUNIX_MOUSE_H
#define MYUNIX_MOUSE_H

#include <stdbool.h>
#include <stdint.h>

/* Called from the IRQ12 path. */
void mouse_interrupt(void);

/* Initialize the PS/2 mouse (enables auxiliary device). */
void mouse_init(void);

/* Read current mouse state (non-blocking). */
struct mouse_state {
    int32_t x;       /* absolute cursor X (clamped to screen) */
    int32_t y;       /* absolute cursor Y (clamped to screen) */
    int32_t dx;      /* raw delta this packet */
    int32_t dy;      /* raw delta this packet */
    uint8_t buttons; /* bit 0=left, 1=right, 2=middle */
    bool    valid;   /* at least one packet received */
};

void mouse_get_state(struct mouse_state *out);

#endif /* MYUNIX_MOUSE_H */
