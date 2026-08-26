#include <io.h>
#include <libk.h>
#include <limine.h>
#include <string.h>
#include <tty.h>

#include "font.h"

/* COM1 mirror so headless QEMU (-serial stdio) still shows the console. */
#define COM1 0x3F8

static volatile uint8_t *fb = NULL;
static size_t pitch = 0, bpp = 0;
static int cols = 0, rows = 0;
static int cur_x = 0, cur_y = 0;
static uint32_t mode_w = 0, mode_h = 0, mode_bpp = 0;

#define CELL_W 8
#define CELL_H 16
#define GLYPH_H 8

#define FG_COLOR 0xFFC8C8C8ULL
#define BG_COLOR 0xFF000000ULL

static void draw_pixel(int px, int py, uint32_t color) {
    volatile uint32_t *p =
        (volatile uint32_t *)(fb + py * pitch + px * (bpp / 8));
    *p = color;
}

static void draw_cell(int cx, int cy, char ch, bool cursor) {
    if (ch < FONT_FIRST || ch > FONT_LAST) ch = '.';

    const uint8_t *glyph = font8x8[ch - FONT_FIRST];
    int ox = cx * CELL_W;
    int oy = cy * CELL_H;

    for (int r = 0; r < CELL_H; r++) {
        uint8_t bits = (r < GLYPH_H) ? glyph[r] : 0x00;
        for (int c = 0; c < CELL_W; c++) {
            /* Font data is LSB-left: bit 0 is the leftmost pixel. */
            bool on = bits & (1u << c);
            draw_pixel(ox + c, oy + r, on ? FG_COLOR : BG_COLOR);
        }
    }

    /* Cursor underline */
    if (cursor) {
        for (int c = 1; c < CELL_W - 1; c++) {
            draw_pixel(ox + c, oy + CELL_H - 2, FG_COLOR);
            draw_pixel(ox + c, oy + CELL_H - 3, FG_COLOR);
        }
    }
}

static void erase_cursor(void) {
    if (cur_x >= cols || cur_y >= rows) return;
    draw_cell(cur_x, cur_y, ' ', false);
}

static void show_cursor(void) {
    if (cur_x >= cols || cur_y >= rows) return;
    draw_cell(cur_x, cur_y, ' ', true);
}

static void scroll_up(void) {
    memmove((void *)fb,
            (const void *)(fb + pitch * CELL_H),
            pitch * CELL_H * (size_t)(rows - 1));

    volatile uint32_t *last_row =
        (volatile uint32_t *)(fb + pitch * CELL_H * (size_t)(rows - 1));
    size_t line_px = pitch * CELL_H / (bpp / 8);
    for (size_t i = 0; i < line_px; i++) last_row[i] = (uint32_t)BG_COLOR;
}

void tty_init(struct limine_framebuffer *f) {
    if (f == NULL || f->bpp != 32) {
        kprintf("[TTY] No usable framebuffer, console stays serial-only\n");
        return;
    }

    fb = (volatile uint8_t *)f->address;
    pitch = f->pitch;
    bpp = f->bpp;
    mode_w = f->width;
    mode_h = f->height;
    mode_bpp = f->bpp;
    cols = (int)(f->width / CELL_W);
    rows = (int)(f->height / CELL_H);

    tty_clear();
    kprintf("[TTY] %dx%d text console (%dx%d cells)\n",
            cols * CELL_W, rows * CELL_H, cols, rows);
}

void tty_clear(void) {
    if (fb == NULL) return;

    size_t total = pitch * CELL_H * (size_t)rows;
    volatile uint32_t *p = (volatile uint32_t *)fb;
    for (size_t i = 0; i < total / 4; i++) p[i] = (uint32_t)BG_COLOR;

    cur_x = cur_y = 0;
}

void tty_get_mode(uint32_t *w, uint32_t *h, uint32_t *bpp_out) {
    *w = mode_w;
    *h = mode_h;
    *bpp_out = mode_bpp;
}

void tty_putc(char c) {
    /* Serial mirror: keep headless runs informative. */
    if (c == '\n') outb(COM1, '\r');
    outb(COM1, (uint8_t)c);

    if (fb == NULL) return;

    erase_cursor();

    switch (c) {
        case '\n':
            cur_x = 0;
            cur_y++;
            break;
        case '\r':
            cur_x = 0;
            break;
        case '\b':
            if (cur_x > 0) {
                cur_x--;
                draw_cell(cur_x, cur_y, ' ', false);
            }
            break;
        case '\t':
            cur_x = (cur_x + 8) & ~7;
            break;
        default:
            draw_cell(cur_x, cur_y, c, false);
            cur_x++;
            break;
    }

    if (cur_x >= cols) {
        cur_x = 0;
        cur_y++;
    }
    while (cur_y >= rows) {
        scroll_up();
        cur_y--;
    }

    show_cursor();
}

void tty_write(const char *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        tty_putc(buf[i]);
    }
}
