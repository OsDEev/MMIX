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

/* ANSI 16-color palette (ARGB) */
static const uint32_t ansi_palette[16] = {
    0xFF000000, 0xFFAA0000, 0xFF00AA00, 0xFFAA5500,
    0xFF0000AA, 0xFFAA00AA, 0xFF00AAAA, 0xFFAAAAAA,
    0xFF555555, 0xFFFF5555, 0xFF55FF55, 0xFFFFFF55,
    0xFF5555FF, 0xFFFF55FF, 0xFF55FFFF, 0xFFFFFFFF,
};

static uint32_t g_fg = 0xFFAAAAAA; /* light gray */
static uint32_t g_bg = 0xFF000000;

/* ANSI escape sequence parser state */
static int ansi_state = 0; /* 0=text 1=saw-ESC 2=in-CSI */
#define ANSI_MAX_PARAMS 8
static int csi_param[ANSI_MAX_PARAMS];
static int csi_count;
static int csi_cur = -1;

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
            draw_pixel(ox + c, oy + r, on ? g_fg : g_bg);
        }
    }

    /* Cursor underline */
    if (cursor) {
        for (int c = 1; c < CELL_W - 1; c++) {
            draw_pixel(ox + c, oy + CELL_H - 2, g_fg);
            draw_pixel(ox + c, oy + CELL_H - 3, g_fg);
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
    for (size_t i = 0; i < line_px; i++) last_row[i] = g_bg;
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
    for (size_t i = 0; i < total / 4; i++) p[i] = g_bg;

    cur_x = cur_y = 0;
}

void *tty_fb(void) { return (void *)fb; }
size_t tty_pitch(void) { return pitch; }
uint32_t tty_bpp(void) { return bpp; }
uint32_t tty_width(void) { return (uint32_t)cols * CELL_W; }
uint32_t tty_height(void) { return (uint32_t)rows * CELL_H; }

void tty_get_mode(uint32_t *w, uint32_t *h, uint32_t *bpp_out) {
    *w = mode_w;
    *h = mode_h;
    *bpp_out = mode_bpp;
}

static void csi_dispatch(int final) {
    int p0 = (csi_count >= 0 && csi_param[0] >= 0) ? csi_param[0] : 1;
    int p1 = (csi_count >= 1 && csi_param[1] >= 0) ? csi_param[1] : 1;

    switch (final) {
        case 'J': /* 2J = clear screen */
            if (p0 == 2) tty_clear();
            break;
        case 'H': /* row;col home (1-based) */
            erase_cursor();
            cur_y = (p0 ? p0 - 1 : 0);
            cur_x = (p1 ? p1 - 1 : 0);
            if (cur_x >= cols) cur_x = cols - 1;
            if (cur_y >= rows) cur_y = rows - 1;
            break;
        case 'K': /* erase to end of line */
            for (int x = cur_x; x < cols; x++) draw_cell(x, cur_y, ' ', false);
            break;
        case 'A': erase_cursor(); cur_y -= p0; if (cur_y < 0) cur_y = 0; break;
        case 'B': erase_cursor(); cur_y += p0; if (cur_y >= rows) cur_y = rows - 1; break;
        case 'C': erase_cursor(); cur_x += p0; if (cur_x >= cols) cur_x = cols - 1; break;
        case 'D': erase_cursor(); cur_x -= p0; if (cur_x < 0) cur_x = 0; break;
        case 'm': { /* SGR */
            int n = csi_count + 1;
            if (n == 1 && csi_param[0] < 0) { g_fg = ansi_palette[7]; g_bg = ansi_palette[0]; break; }
            for (int i = 0; i < n; i++) {
                int v = csi_param[i];
                if (v < 0) v = 0;
                if (v == 0)      { g_fg = ansi_palette[7]; g_bg = ansi_palette[0]; }
                else if (v == 1) { if ((g_fg & 0xFF) < 0x80) g_fg |= 0x00555555; }
                else if (v >= 30 && v <= 37) g_fg = ansi_palette[v - 30];
                else if (v >= 90 && v <= 97) g_fg = ansi_palette[v - 90 + 8];
                else if (v >= 40 && v <= 47) g_bg = ansi_palette[v - 40];
                else if (v >= 100 && v <= 107) g_bg = ansi_palette[v - 100 + 8];
                else if (v == 39) g_fg = ansi_palette[7];
                else if (v == 49) g_bg = ansi_palette[0];
            }
            break;
        }
        default:
            break;
    }
}

static void tty_ansi_feed(char c) {
    if (ansi_state == 1) { /* saw ESC */
        if (c == '[') {
            ansi_state = 2;
            csi_count = 0;
            csi_cur = -1;
            for (int i = 0; i < ANSI_MAX_PARAMS; i++) csi_param[i] = -1;
        } else {
            ansi_state = 0;
        }
        return;
    }

    /* in CSI */
    if (c >= '0' && c <= '9') {
        if (csi_cur < 0) { csi_cur = csi_count; if (csi_cur < ANSI_MAX_PARAMS) csi_param[csi_cur] = 0; }
        if (csi_cur < ANSI_MAX_PARAMS) csi_param[csi_cur] = csi_param[csi_cur] * 10 + (c - '0');
        return;
    }
    if (c == ';') {
        csi_cur = csi_count;
        if (csi_cur < ANSI_MAX_PARAMS - 1) csi_count++;
        return;
    }
    if (c >= 0x40 && c <= 0x7E) {
        csi_dispatch(c);
    }
    ansi_state = 0;
}

void tty_putc(char c) {
    /* Serial mirror: keep headless runs informative. */
    if (c == '\n') outb(COM1, '\r');
    outb(COM1, (uint8_t)c);

    if (fb == NULL) return;

    if (c == 0x1B) { ansi_state = 1; return; }
    if (ansi_state != 0) { tty_ansi_feed(c); return; }

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
