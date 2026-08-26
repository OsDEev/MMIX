/*
 * desktop -- MMIX graphical desktop (BETA), mouse-driven.
 *
 * Maps the framebuffer into userspace, renders a desktop with icons,
 * widgets, and a software cursor. Clickable icons, keyboard shortcuts.
 */
#include "libc.h"

#define FB_W 800
#define FB_H 600

#define CLR_BG       0xFF1A1A2E
#define CLR_TASKBAR  0xFF16213E
#define CLR_ACCENT   0xFF0F3460
#define CLR_HIGHLIGHT 0xFF533483
#define CLR_WIDGET   0xFF1E293B
#define CLR_DARK     0xFF0D1117
#define CLR_GREEN    0xFF00CC66
#define CLR_RED      0xFFCC3333
#define CLR_YELLOW   0xFFCCAA00
#define CLR_BLUE     0xFF3399FF
#define CLR_CYAN     0xFF00CCCC
#define CLR_ORANGE   0xFFCC7700
#define CLR_PINK     0xFFFF66AA
#define CLR_WHITE    0xFFFFFFFF
#define CLR_BLACK    0xFF000000

/* Framebuffer (mapped into userspace) */
static volatile uint32_t *fb = NULL;
static int fb_pitch; /* in uint32_t */

static inline void fbp(int x, int y, uint32_t c) {
    if (x >= 0 && x < FB_W && y >= 0 && y < FB_H)
        fb[y * fb_pitch + x] = c;
}

static inline uint32_t fbget(int x, int y) {
    if (x >= 0 && x < FB_W && y >= 0 && y < FB_H)
        return fb[y * fb_pitch + x];
    return CLR_BG;
}

static void draw_fill(int x, int y, int w, int h, uint32_t c) {
    gfx_fill_rect(x, y, w, h, c);
}

static void draw_box(int x, int y, int w, int h, uint32_t border, uint32_t fill) {
    gfx_fill_rect(x, y, w, h, fill);
    gfx_rect(x, y, w, h, border);
}

/* --- Icons ---------------------------------------------------------------- */

struct icon {
    int x, y, w, h;
    uint32_t color;
    int action;
};

static struct icon icons[] = {
    {  40, 80, 110, 110, CLR_GREEN,   1 },
    { 170, 80, 110, 110, CLR_BLUE,    2 },
    { 300, 80, 110, 110, CLR_YELLOW,  3 },
    { 430, 80, 110, 110, CLR_CYAN,    4 },
    { 560, 80, 110, 110, CLR_PINK,    5 },
    {  40, 210, 110, 110, CLR_ORANGE,  6 },
    { 170, 210, 110, 110, CLR_RED,     7 },
    { 300, 210, 110, 110, CLR_HIGHLIGHT, 8 },
};
#define NUM_ICONS (sizeof(icons) / sizeof(icons[0]))

/* --- Cursor --------------------------------------------------------------- */

#define CURSOR_W 10
#define CURSOR_H 13

static const uint8_t cursor_bmp[13] = {
    0b11000000,
    0b11100000,
    0b11110000,
    0b11111000,
    0b11111100,
    0b11111110,
    0b11111111,
    0b11111011,
    0b11100000,
    0b11010000,
    0b11001000,
    0b10000100,
    0b00000000,
};

static int cursor_x = 400, cursor_y = 300;
static int prev_cx = -1, prev_cy = -1;
static uint32_t saved_bg[CURSOR_W * CURSOR_H];
static int hovered_icon = -1;

static void save_bg(int cx, int cy) {
    for (int r = 0; r < CURSOR_H; r++)
        for (int c = 0; c < CURSOR_W; c++)
            saved_bg[r * CURSOR_W + c] = fbget(cx + c, cy + r);
}

static void restore_bg(int cx, int cy) {
    for (int r = 0; r < CURSOR_H; r++)
        for (int c = 0; c < CURSOR_W; c++)
            fbp(cx + c, cy + r, saved_bg[r * CURSOR_W + c]);
}

static void draw_cursor_at(int cx, int cy) {
    for (int r = 0; r < CURSOR_H; r++) {
        for (int c = 0; c < CURSOR_W; c++) {
            if (cursor_bmp[r] & (1 << (7 - c))) {
                /* White arrow with black edge */
                int sx = cx + c, sy = cy + r;
                if (sx >= 0 && sx < FB_W && sy >= 0 && sy < FB_H) {
                    int edge = (c == 0 || r == 0 ||
                        !(cursor_bmp[r] & (1 << (7 - c + 1))) ||
                        !(cursor_bmp[r > 0 ? r - 1 : r] & (1 << (7 - c))));
                    fb[sy * fb_pitch + sx] = edge ? 0xFF333333 : CLR_WHITE;
                }
            }
        }
    }
}

static void move_cursor(int nx, int ny) {
    if (prev_cx >= 0) restore_bg(prev_cx, prev_cy);
    prev_cx = cursor_x;
    prev_cy = cursor_y;
    cursor_x = nx;
    cursor_y = ny;
    save_bg(cursor_x, cursor_y);
    draw_cursor_at(cursor_x, cursor_y);
}

/* --- Hit testing ---------------------------------------------------------- */

static int hit_icon(int mx, int my) {
    for (int i = 0; i < (int)NUM_ICONS; i++) {
        struct icon *ic = &icons[i];
        if (mx >= ic->x && mx < ic->x + ic->w &&
            my >= ic->y && my < ic->y + ic->h)
            return i;
    }
    return -1;
}

/* --- Drawing -------------------------------------------------------------- */

static void draw_icon(int i, int hover) {
    struct icon *ic = &icons[i];
    uint32_t border = hover ? CLR_WHITE : ic->color;
    int sh = hover ? 6 : 3;
    draw_fill(ic->x + sh, ic->y + sh, ic->w, ic->h, 0x30000000);
    draw_box(ic->x, ic->y, ic->w, ic->h, border, CLR_WIDGET);
    draw_fill(ic->x + 1, ic->y + 1, ic->w - 2, 8, ic->color);
    draw_fill(ic->x + 15, ic->y + 25, ic->w - 30, 4, ic->color);
    draw_fill(ic->x + 15, ic->y + 35, ic->w - 30, 4, ic->color);
    draw_fill(ic->x + 15, ic->y + 45, ic->w - 40, 4, ic->color);
}

static void draw_taskbar(int active) {
    int y = FB_H - 40;
    draw_fill(0, y, FB_W, 40, CLR_TASKBAR);
    draw_fill(0, y, FB_W, 2, CLR_ACCENT);
    for (int i = 0; i < (int)NUM_ICONS && i < 8; i++) {
        int bx = 10 + i * 50;
        draw_fill(bx, y + 6, 36, 28, (i == active) ? CLR_HIGHLIGHT : CLR_ACCENT);
        draw_fill(bx + 14, y + 14, 8, 8, icons[i].color);
    }
}

static void draw_panel(void) {
    draw_box(580, 80, 200, 180, CLR_ACCENT, CLR_WIDGET);

    /* Clock bars */
    draw_box(580, 280, 200, 70, CLR_ACCENT, CLR_WIDGET);
    struct mmix_timeval tv;
    systime(&tv);
    draw_fill(590, 295, tv.hour * 3, 12, CLR_GREEN);
    draw_fill(590, 312, tv.min * 3, 12, CLR_BLUE);
    draw_fill(590, 329, tv.sec * 3, 12, CLR_CYAN);

    /* Memory bar */
    struct mmix_sysinfo si;
    sysinfo(&si);
    uint32_t used = si.total_ram_kb - si.free_ram_kb;
    int bar = 180;
    int fill = (si.total_ram_kb > 0) ? (int)((long)bar * used / si.total_ram_kb) : 0;
    draw_fill(590, 100, bar, 20, CLR_DARK);
    draw_fill(590, 100, fill, 20, CLR_RED);
    draw_fill(590 + fill, 100, bar - fill, 20, CLR_GREEN);

    draw_fill(590, 130, 120, 8, CLR_GREEN);
    draw_fill(590, 145, 90, 8, CLR_BLUE);
    int ud = si.uptime_s > 170 ? 170 : (int)si.uptime_s;
    draw_fill(590, 160, ud, 8, CLR_YELLOW);

    for (int i = 0; i < 5; i++) {
        draw_fill(590 + i * 36, 180, 30, 30, CLR_DARK);
        uint32_t dc[] = { CLR_GREEN, CLR_BLUE, CLR_YELLOW, CLR_CYAN, CLR_RED };
        draw_fill(600 + i * 36, 190, 10, 10, dc[i]);
    }
}

static void draw_desktop_full(void) {
    gfx_fill_rect(0, 0, FB_W, FB_H, CLR_BG);
    draw_fill(0, 0, FB_W, 55, CLR_ACCENT);
    draw_fill(0, 53, FB_W, 2, CLR_HIGHLIGHT);
    for (int i = 0; i < 55; i += 3) draw_fill(0, i, FB_W, 1, CLR_HIGHLIGHT);
    draw_fill(20, 18, 80, 16, CLR_GREEN);
    draw_fill(200, 18, 60, 16, CLR_CYAN);

    for (int i = 0; i < (int)NUM_ICONS; i++) draw_icon(i, i == hovered_icon);
    draw_panel();
    draw_fill(FB_W / 2 - 130, FB_H - 70, 260, 22, CLR_ACCENT);
    draw_fill(FB_W / 2 - 128, FB_H - 68, 4, 18, CLR_YELLOW);
    draw_fill(FB_W / 2 - 120, FB_H - 68, 4, 18, CLR_YELLOW);
    draw_fill(FB_W / 2 - 112, FB_H - 68, 4, 18, CLR_YELLOW);
    draw_taskbar(-1);
}

/* --- Demo screens --------------------------------------------------------- */

static void demo_proc(void) {
    gfx_fill_rect(0, 0, FB_W, FB_H, CLR_DARK);
    draw_fill(0, 0, FB_W, 50, CLR_ACCENT);
    draw_fill(0, 48, FB_W, 2, CLR_GREEN);
    draw_fill(20, 16, 50, 16, CLR_GREEN);
    int y = 70;
    uint32_t c[] = { CLR_GREEN, CLR_RED, CLR_BLUE, CLR_YELLOW, CLR_CYAN, CLR_HIGHLIGHT };
    for (int i = 0; i < 6; i++) {
        draw_box(40, y, 700, 40, CLR_ACCENT, CLR_WIDGET);
        draw_fill(50, y + 10, 12, 20, c[i]);
        draw_fill(70, y + 12, 100, 16, CLR_DARK);
        draw_fill(180, y + 12, 180, 16, CLR_DARK);
        draw_fill(370, y + 12, 60, 16, CLR_DARK);
        draw_fill(440, y + 12, 30 + ((i+1)*47)%280, 16, c[i]);
        y += 50;
    }
    draw_taskbar(0);
}

static void demo_info(void) {
    gfx_fill_rect(0, 0, FB_W, FB_H, CLR_DARK);
    draw_fill(0, 0, FB_W, 50, CLR_ACCENT);
    draw_fill(0, 48, FB_W, 2, CLR_CYAN);
    draw_fill(20, 16, 60, 16, CLR_CYAN);
    int y = 80;
    uint32_t lc[] = { CLR_GREEN, CLR_BLUE, CLR_YELLOW, CLR_CYAN };
    for (int i = 0; i < 4; i++) {
        draw_box(40, y, 340, 50, CLR_ACCENT, CLR_WIDGET);
        draw_fill(50, y + 12, 40+i*15, 8, lc[i]);
        draw_fill(50, y + 26, 60+i*10, 8, lc[i]);
        draw_fill(360, y + 12, 16, 16, lc[i]);
        y += 65;
    }
    draw_box(420, 80, 340, 340, CLR_ACCENT, CLR_WIDGET);
    for (int i = 0; i < 10; i++) draw_fill(440, 100 + i * 30, 300, 18, CLR_DARK);
    draw_taskbar(1);
}

static void demo_gfx(void) {
    gfx_fill_rect(0, 0, FB_W, FB_H, CLR_BG);
    for (int i = 0; i < 15; i++) {
        int s = 300 - i * 18;
        if (s <= 0) break;
        uint32_t cl[] = { CLR_RED, CLR_GREEN, CLR_BLUE, CLR_YELLOW, CLR_CYAN, CLR_HIGHLIGHT };
        draw_fill(400-s/2, 300-s/2, s, s, cl[i%6]);
    }
    for (int i = 0; i < 80; i++) {
        gfx_line(i*10, 0, 0, i*7, CLR_ACCENT);
        gfx_line(FB_W-i*10, FB_H, FB_W, FB_H-i*7, CLR_HIGHLIGHT);
    }
    for (int r = 20; r <= 100; r += 20)
        gfx_circle(400, 300, r, (r%40==0) ? CLR_CYAN : CLR_ACCENT);
    draw_taskbar(3);
}

static void demo_color(void) {
    gfx_fill_rect(0, 0, FB_W, FB_H, CLR_BG);
    uint32_t p[] = { 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00,
                     0xFFFF00FF, 0xFF00FFFF, 0xFF884400, 0xFF8800FF };
    for (int y = 0; y < FB_H; y += 60)
        for (int x = 0; x < FB_W; x += 80)
            draw_fill(x+2, y+2, 76, 56, p[((x/80)+(y/60))%8]);
    draw_taskbar(5);
}

static void demo_about(void) {
    gfx_fill_rect(0, 0, FB_W, FB_H, CLR_DARK);
    draw_fill(0, 0, FB_W, 50, CLR_ACCENT);
    draw_fill(0, 48, FB_W, 2, CLR_RED);
    draw_fill(20, 16, 45, 16, CLR_RED);
    draw_box(150, 100, 500, 400, CLR_ACCENT, CLR_WIDGET);
    draw_fill(152, 102, 496, 40, CLR_RED);
    draw_fill(250, 160, 300, 4, CLR_WHITE);
    draw_fill(250, 175, 250, 4, CLR_RED);
    draw_fill(250, 200, 200, 4, CLR_YELLOW);
    draw_fill(250, 240, 300, 4, CLR_CYAN);
    draw_fill(250, 260, 280, 4, CLR_GREEN);
    draw_fill(250, 280, 200, 4, CLR_BLUE);
    draw_fill(250, 320, 300, 4, CLR_WHITE);
    draw_fill(250, 340, 250, 4, CLR_YELLOW);
    draw_fill(250, 360, 150, 4, CLR_CYAN);
    draw_fill(250, 380, 200, 4, CLR_GREEN);
    draw_taskbar(6);
}

static void demo_mem(void) {
    gfx_fill_rect(0, 0, FB_W, FB_H, CLR_DARK);
    draw_fill(0, 0, FB_W, 50, CLR_ACCENT);
    draw_fill(0, 48, FB_W, 2, CLR_YELLOW);
    draw_fill(20, 16, 40, 16, CLR_YELLOW);
    int y = 70;
    uint32_t sc[] = { CLR_RED, CLR_GREEN, CLR_BLUE, CLR_YELLOW,
                      CLR_CYAN, CLR_HIGHLIGHT, CLR_ORANGE, CLR_PINK };
    for (int i = 0; i < 8; i++) {
        int w = 100 + (i*67)%400;
        draw_box(40, y, w, 30, CLR_ACCENT, CLR_WIDGET);
        draw_fill(42, y+2, w-4, 26, sc[i]);
        draw_fill(42, y+2, (w-4)/3, 26, CLR_DARK);
        y += 40;
    }
    for (int i = 0; i < 8; i++)
        gfx_fill_circle(600+(i*17)%100-50, 300+(i*23)%100-50, 20, sc[i]);
    draw_taskbar(2);
}

/* --- Main ----------------------------------------------------------------- */

static void on_click(int idx) {
    switch (icons[idx].action) {
        case 1: demo_proc(); break;
        case 2: demo_info(); break;
        case 3: demo_mem();  break;
        case 4: demo_gfx();  break;
        case 5: demo_info(); break;
        case 6: demo_color();break;
        case 7: demo_about();break;
        case 8: demo_gfx();  break;
    }
}

int main(void) {
    /* Map framebuffer into our address space */
    fb = gfx_fb_mmap(0x60000000);
    if (fb == NULL) {
        print("desktop: failed to map framebuffer\n");
        return 1;
    }
    /* Pitch is in bytes; we treat it as uint32_t count */
    struct mmix_sysinfo si;
    sysinfo(&si);
    fb_pitch = gfx_get_pitch();
    if (fb_pitch <= 0) fb_pitch = FB_W;

    draw_desktop_full();
    prev_cx = cursor_x;
    prev_cy = cursor_y;
    save_bg(cursor_x, cursor_y);
    draw_cursor_at(cursor_x, cursor_y);

    int last_mx = cursor_x, last_my = cursor_y;
    uint8_t last_btn = 0;

    for (;;) {
        struct mmix_mouse ms;
        int has_mouse = (mouse_read(&ms) == 0);

        char c;
        long kr = read(0, &c, 1);

        if (kr > 0) {
            if (c == 0x1B) break;
            if (c >= '1' && c <= '8') {
                on_click(c - '1');
                /* Redraw desktop after demo */
                draw_desktop_full();
                prev_cx = -1;
                save_bg(cursor_x, cursor_y);
                draw_cursor_at(cursor_x, cursor_y);
                continue;
            }
        }

        if (has_mouse && (ms.x != last_mx || ms.y != last_my)) {
            move_cursor(ms.x, ms.y);
            last_mx = ms.x;
            last_my = ms.y;

            int new_hover = hit_icon(ms.x, ms.y);
            if (new_hover != hovered_icon) {
                int old = hovered_icon;
                hovered_icon = new_hover;
                if (old >= 0) draw_icon(old, false);
                if (new_hover >= 0) draw_icon(new_hover, true);
                /* Restore cursor on top */
                save_bg(cursor_x, cursor_y);
                draw_cursor_at(cursor_x, cursor_y);
            }
        }

        if (has_mouse && (ms.buttons & 1) && !(last_btn & 1)) {
            int idx = hit_icon(ms.x, ms.y);
            if (idx >= 0) {
                on_click(idx);
                draw_desktop_full();
                prev_cx = -1;
                save_bg(cursor_x, cursor_y);
                draw_cursor_at(cursor_x, cursor_y);
            }
        }
        if (has_mouse) last_btn = ms.buttons;

        yield();
    }

    gfx_fill_rect(0, 0, FB_W, FB_H, CLR_BLACK);
    return 0;
}
