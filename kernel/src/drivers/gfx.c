#include <gfx.h>
#include <tty.h>

/* Direct framebuffer drawing primitives (bypass the text grid). */

static int clamp_x(int x) {
    uint32_t w = tty_width();
    if (x < 0) return 0;
    if (x >= (int)w) return (int)w - 1;
    return x;
}

static int clamp_y(int y) {
    uint32_t h = tty_height();
    if (y < 0) return 0;
    if (y >= (int)h) return (int)h - 1;
    return y;
}

static void px(int x, int y, uint32_t color) {
    volatile uint32_t *fb = (volatile uint32_t *)tty_fb();
    size_t pitch = tty_pitch();
    fb[y * (pitch / 4) + x] = color;
}

void gfx_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    int x0 = clamp_x(x), y0 = clamp_y(y);
    int x1 = clamp_x(x + w - 1), y1 = clamp_y(y + h - 1);
    for (int yy = y0; yy <= y1; yy++) {
        for (int xx = x0; xx <= x1; xx++) {
            px(xx, yy, color);
        }
    }
}

void gfx_rect(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    gfx_fill_rect(x, y, w, 1, color);
    gfx_fill_rect(x, y + h - 1, w, 1, color);
    gfx_fill_rect(x, y, 1, h, color);
    gfx_fill_rect(x + w - 1, y, 1, h, color);
}

void gfx_line(int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;

    int err = dx - dy;
    for (;;) {
        if (x0 >= 0 && y0 >= 0 &&
            x0 < (int)tty_width() && y0 < (int)tty_height()) {
            px(x0, y0, color);
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

void gfx_circle(int cx, int cy, int r, uint32_t color) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        px(cx + x, cy + y, color); px(cx - x, cy + y, color);
        px(cx + y, cy + x, color); px(cx - y, cy + x, color);
        px(cx - x, cy - y, color); px(cx + x, cy - y, color);
        px(cx - y, cy - x, color); px(cx + y, cy - x, color);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

static int isqrt(int v) {
    int res = 0, bit = 1 << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= res + bit) {
            v -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

void gfx_fill_circle(int cx, int cy, int r, uint32_t color) {
    for (int dy = -r; dy <= r; dy++) {
        int dx = r * r - dy * dy;
        if (dx < 0) continue;
        dx = isqrt(dx);
        gfx_fill_rect(cx - dx, cy + dy, dx * 2 + 1, 1, color);
    }
}

void gfx_clear(uint32_t color) {
    gfx_fill_rect(0, 0, (int)tty_width(), (int)tty_height(), color);
}
