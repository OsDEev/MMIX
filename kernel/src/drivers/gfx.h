#ifndef MYUNIX_GFX_H
#define MYUNIX_GFX_H

#include <stdint.h>

void gfx_fill_rect(int x, int y, int w, int h, uint32_t color);
void gfx_rect(int x, int y, int w, int h, uint32_t color);
void gfx_line(int x0, int y0, int x1, int y1, uint32_t color);
void gfx_circle(int cx, int cy, int r, uint32_t color);
void gfx_fill_circle(int cx, int cy, int r, uint32_t color);
void gfx_clear(uint32_t color);

#endif /* MYUNIX_GFX_H */
