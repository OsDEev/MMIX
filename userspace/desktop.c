/*
 * desktop -- MMIX graphical desktop (BETA).
 *
 * Draws a desktop background with a taskbar, app icons, and system info.
 * Keyboard-driven: press a number to launch a demo action, ESC exits.
 */
#include "libc.h"

#define FB_W 800
#define FB_H 600

/* Colors */
#define CLR_BG      0xFF1A1A2E
#define CLR_TASKBAR 0xFF16213E
#define CLR_TASKTXT 0xFFAAAAAA
#define CLR_TITLE   0xFFFFFFFF
#define CLR_ACCENT  0xFF0F3460
#define CLR_HIGHLIGHT 0xFF533483
#define CLR_GREEN   0xFF00CC66
#define CLR_RED     0xFFCC3333
#define CLR_YELLOW  0xFFCCAA00
#define CLR_BLUE    0xFF3399FF
#define CLR_CYAN    0xFF00CCCC
#define CLR_DARK    0xFF0D1117
#define CLR_WIDGET  0xFF1E293B

struct icon {
    int x, y, w, h;
    uint32_t color;
    const char *label;
    const char *desc;
};

static struct icon icons[] = {
    { 60,  100, 100, 100, CLR_GREEN,   "proc",    "Process list" },
    { 180, 100, 100, 100, CLR_BLUE,    "info",    "System info" },
    { 300, 100, 100, 100, CLR_YELLOW,  "mem",     "Memory map" },
    { 420, 100, 100, 100, CLR_RED,     "time",    "RTC clock" },
    { 540, 100, 100, 100, CLR_CYAN,    "gfx",     "GFX demo" },
    { 60,  240, 100, 100, CLR_HIGHLIGHT,"disk",   "Disk info" },
    { 180, 240, 100, 100, 0xFF6666FF,  "calc",    "Calculator" },
    { 300, 240, 100, 100, 0xFFAA6633,  "color",   "Color test" },
};
#define NUM_ICONS (sizeof(icons) / sizeof(icons[0]))

static void draw_rect(int x, int y, int w, int h, uint32_t color) {
    gfx_fill_rect(x, y, w, h, color);
}

static void draw_box(int x, int y, int w, int h, uint32_t border, uint32_t fill) {
    gfx_fill_rect(x, y, w, h, fill);
    gfx_rect(x, y, w, h, border);
}

static void draw_text_bg(const char *s, int x, int y, uint32_t fg, uint32_t bg) {
    (void)s; (void)x; (void)y; (void)fg; (void)bg;
    /* We use the kernel tty for text; gfx primitives for shapes. */
}

static void draw_clock(int x, int y) {
    struct mmix_timeval tv;
    systime(&tv);

    /* Draw clock widget background */
    draw_box(x, y, 180, 60, CLR_ACCENT, CLR_WIDGET);

    /* Draw time using gfx rectangles as digit segments */
    int digits[6];
    digits[0] = tv.hour / 10;
    digits[1] = tv.hour % 10;
    digits[2] = tv.min / 10;
    digits[3] = tv.min % 10;
    digits[4] = tv.sec / 10;
    digits[5] = tv.sec % 10;

    /* Simple bar graph representation of time */
    for (int i = 0; i < 6; i++) {
        int bx = x + 10 + i * 28;
        int bh = digits[i] * 4 + 4;
        uint32_t c = (i < 4) ? CLR_GREEN : CLR_BLUE;
        draw_rect(bx, y + 50 - bh, 20, bh, c);
    }
}

static void draw_memory_bar(int x, int y, int w) {
    struct mmix_sysinfo si;
    sysinfo(&si);

    draw_box(x, y, w, 30, CLR_ACCENT, CLR_WIDGET);

    uint32_t used = si.total_ram_kb - si.free_ram_kb;
    int bar_w = (int)((long)w - 4);
    int fill_w = 0;
    if (si.total_ram_kb > 0)
        fill_w = (int)((long)bar_w * used / si.total_ram_kb);

    draw_rect(x + 2, y + 2, bar_w, 26, CLR_DARK);
    if (fill_w > 0)
        draw_rect(x + 2, y + 2, fill_w, 26, CLR_RED);

    /* Free portion */
    if (fill_w < bar_w)
        draw_rect(x + 2 + fill_w, y + 2, bar_w - fill_w, 26, CLR_GREEN);
}

static void draw_taskbar(int selected) {
    int y = FB_H - 40;

    /* Taskbar background */
    draw_rect(0, y, FB_W, 40, CLR_TASKBAR);

    /* Separator line */
    draw_rect(0, y, FB_W, 2, CLR_ACCENT);

    /* Mini icons in taskbar */
    for (int i = 0; i < (int)NUM_ICONS && i < 8; i++) {
        int bx = 10 + i * 50;
        uint32_t c = (i == selected) ? CLR_HIGHLIGHT : CLR_ACCENT;
        draw_rect(bx, y + 6, 36, 28, c);
        /* Small colored dot as icon indicator */
        draw_rect(bx + 14, y + 14, 8, 8, icons[i].color);
    }

    /* Version label on right side */
    draw_rect(FB_W - 120, y + 10, 110, 20, CLR_ACCENT);
}

static void draw_desktop(void) {
    gfx_clear(CLR_BG);

    /* Title banner */
    draw_rect(0, 0, FB_W, 60, CLR_ACCENT);
    draw_rect(0, 58, FB_W, 2, CLR_HIGHLIGHT);

    /* Draw decorative lines */
    for (int i = 0; i < 60; i += 4) {
        draw_rect(0, i, FB_W, 1, CLR_HIGHLIGHT);
    }

    /* Status text area (using colored rectangles as "text") */
    draw_rect(20, 18, 4, 20, CLR_GREEN);
    draw_rect(28, 18, 4, 20, CLR_GREEN);
    draw_rect(36, 18, 4, 20, CLR_GREEN);
    draw_rect(44, 18, 4, 20, CLR_GREEN);
    draw_rect(52, 18, 4, 20, CLR_GREEN);
    draw_rect(60, 18, 4, 20, CLR_GREEN);
    draw_rect(68, 18, 4, 20, CLR_GREEN);
    draw_rect(76, 18, 4, 20, CLR_GREEN);

    /* MMIX label dots */
    draw_rect(200, 20, 80, 16, CLR_HIGHLIGHT);
    draw_rect(300, 20, 4, 20, CLR_CYAN);
    draw_rect(310, 20, 4, 20, CLR_CYAN);

    /* App icons */
    for (int i = 0; i < (int)NUM_ICONS; i++) {
        struct icon *ic = &icons[i];
        /* Icon shadow */
        draw_rect(ic->x + 4, ic->y + 4, ic->w, ic->h, 0x40000000);
        /* Icon body */
        draw_box(ic->x, ic->y, ic->w, ic->h, ic->color, CLR_WIDGET);
        /* Colored top accent */
        draw_rect(ic->x + 1, ic->y + 1, ic->w - 2, 6, ic->color);
        /* Inner highlight */
        draw_rect(ic->x + 10, ic->y + 20, ic->w - 20, 4, ic->color);
        draw_rect(ic->x + 10, ic->y + 30, ic->w - 20, 4, ic->color);
        draw_rect(ic->x + 10, ic->y + 40, ic->w - 30, 4, ic->color);
    }

    /* Right panel: system info widgets */
    draw_box(600, 100, 180, 160, CLR_ACCENT, CLR_WIDGET);
    draw_rect(602, 102, 176, 20, CLR_ACCENT);

    /* Memory bar */
    draw_memory_bar(610, 140, 160);

    /* Small info boxes */
    draw_box(610, 180, 74, 30, CLR_ACCENT, CLR_DARK);
    draw_rect(615, 185, 64, 4, CLR_GREEN);
    draw_rect(615, 193, 50, 4, CLR_GREEN);

    draw_box(690, 180, 74, 30, CLR_ACCENT, CLR_DARK);
    draw_rect(695, 185, 64, 4, CLR_BLUE);
    draw_rect(695, 193, 45, 4, CLR_BLUE);

    /* Clock */
    draw_clock(600, 280);

    /* Desktop hint at bottom center */
    draw_rect(FB_W / 2 - 150, FB_H - 80, 300, 24, CLR_ACCENT);
    draw_rect(FB_W / 2 - 148, FB_H - 78, 4, 20, CLR_YELLOW);
    draw_rect(FB_W / 2 - 140, FB_H - 78, 4, 20, CLR_YELLOW);
    draw_rect(FB_W / 2 - 132, FB_H - 78, 4, 20, CLR_YELLOW);

    /* Taskbar */
    draw_taskbar(-1);
}

static void demo_gfx(void) {
    gfx_clear(CLR_BG);

    /* Animated-looking concentric rectangles */
    for (int i = 0; i < 20; i++) {
        int x = 300 + i * 8;
        int y = 200 + i * 8;
        int w = 200 - i * 8;
        int h = 150 - i * 8;
        if (w <= 0 || h <= 0) break;
        uint32_t colors[] = { CLR_RED, CLR_GREEN, CLR_BLUE, CLR_YELLOW, CLR_CYAN, CLR_HIGHLIGHT };
        draw_rect(x, y, w, h, colors[i % 6]);
    }

    /* Diagonal lines */
    for (int i = 0; i < 100; i++) {
        gfx_line(i * 8, 0, 0, i * 6, CLR_ACCENT);
        gfx_line(FB_W - i * 8, FB_H, FB_W, FB_H - i * 6, CLR_HIGHLIGHT);
    }

    /* Circles */
    for (int r = 20; r <= 120; r += 20) {
        uint32_t c = (r % 40 == 0) ? CLR_CYAN : CLR_ACCENT;
        gfx_circle(400, 300, r, c);
    }

    draw_taskbar(4);
}

static void demo_color(void) {
    gfx_clear(CLR_BG);
    int colors[] = { 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00,
                     0xFFFF00FF, 0xFF00FFFF, 0xFFFF8800, 0xFF8800FF };
    for (int y = 0; y < FB_H; y += 60) {
        for (int x = 0; x < FB_W; x += 80) {
            int ci = ((x / 80) + (y / 60)) % 8;
            draw_rect(x + 2, y + 2, 76, 56, colors[ci]);
        }
    }
    draw_taskbar(7);
}

static void demo_info(void) {
    gfx_clear(CLR_DARK);

    draw_rect(0, 0, FB_W, 50, CLR_ACCENT);
    draw_rect(0, 48, FB_W, 2, CLR_CYAN);

    /* Title dots */
    draw_rect(20, 16, 60, 16, CLR_CYAN);

    struct mmix_sysinfo si;
    sysinfo(&si);

    /* Draw info blocks as colored rectangles */
    int y = 80;

    /* Total RAM block */
    draw_box(40, y, 340, 50, CLR_ACCENT, CLR_WIDGET);
    draw_rect(50, y + 10, 50, 8, CLR_GREEN);
    draw_rect(50, y + 24, 80, 8, CLR_GREEN);
    draw_rect(50, y + 38, si.total_ram_kb / 100, 4, CLR_GREEN);
    draw_rect(360, y + 10, 16, 16, CLR_GREEN);

    y += 70;

    /* Free RAM block */
    draw_box(40, y, 340, 50, CLR_ACCENT, CLR_WIDGET);
    draw_rect(50, y + 10, 40, 8, CLR_BLUE);
    draw_rect(50, y + 24, 60, 8, CLR_BLUE);
    draw_rect(50, y + 38, si.free_ram_kb / 100, 4, CLR_BLUE);
    draw_rect(360, y + 10, 16, 16, CLR_BLUE);

    y += 70;

    /* Uptime block */
    draw_box(40, y, 340, 50, CLR_ACCENT, CLR_WIDGET);
    draw_rect(50, y + 10, 55, 8, CLR_YELLOW);
    draw_rect(50, y + 24, 70, 8, CLR_YELLOW);
    draw_rect(50, y + 38, si.uptime_s * 2, 4, CLR_YELLOW);
    draw_rect(360, y + 10, 16, 16, CLR_YELLOW);

    y += 70;

    /* FB info block */
    draw_box(40, y, 340, 50, CLR_ACCENT, CLR_WIDGET);
    draw_rect(50, y + 10, 35, 8, CLR_CYAN);
    draw_rect(50, y + 24, 65, 8, CLR_CYAN);
    draw_rect(360, y + 10, 16, 16, CLR_CYAN);

    /* Right panel: decorative */
    draw_box(420, 80, 360, 360, CLR_ACCENT, CLR_WIDGET);
    for (int i = 0; i < 12; i++) {
        draw_rect(440, 100 + i * 28, 320, 20, CLR_DARK);
        draw_rect(442, 102 + i * 28, 4, 16, CLR_GREEN);
    }

    draw_taskbar(1);
}

static void demo_proc(void) {
    gfx_clear(CLR_DARK);
    draw_rect(0, 0, FB_W, 50, CLR_ACCENT);
    draw_rect(0, 48, FB_W, 2, CLR_GREEN);

    draw_rect(20, 16, 50, 16, CLR_GREEN);

    /* Process table as colored rows */
    int y = 70;
    int procs[] = { 1, 2, 3, 4, 5 };
    uint32_t pcolors[] = { CLR_GREEN, CLR_RED, CLR_BLUE, CLR_YELLOW, CLR_CYAN };
    for (int i = 0; i < 5; i++) {
        draw_box(40, y, 700, 40, CLR_ACCENT, CLR_WIDGET);
        draw_rect(50, y + 10, 10, 20, pcolors[i]);
        draw_rect(70, y + 12, 120, 16, CLR_DARK);
        draw_rect(200, y + 12, 200, 16, CLR_DARK);
        draw_rect(420, y + 12, 80, 16, CLR_DARK);
        /* Status bar */
        int bar = 40 + (procs[i] * 37) % 600;
        draw_rect(520, y + 12, bar, 16, pcolors[i]);
        y += 50;
    }

    draw_taskbar(0);
}

static void demo_mem(void) {
    gfx_clear(CLR_DARK);
    draw_rect(0, 0, FB_W, 50, CLR_ACCENT);
    draw_rect(0, 48, FB_W, 2, CLR_YELLOW);
    draw_rect(20, 16, 40, 16, CLR_YELLOW);

    /* Memory map visualization */
    int y = 70;
    uint32_t seg_colors[] = { CLR_RED, CLR_GREEN, CLR_BLUE, CLR_YELLOW,
                              CLR_CYAN, CLR_HIGHLIGHT, CLR_ACCENT, 0xFF6666FF };
    for (int i = 0; i < 8; i++) {
        int w = 100 + (i * 67) % 400;
        draw_box(40, y, w, 30, CLR_ACCENT, CLR_WIDGET);
        draw_rect(42, y + 2, w - 4, 26, seg_colors[i]);
        draw_rect(42, y + 2, (w - 4) / 3, 26, CLR_DARK);
        y += 40;
    }

    /* Pie chart approximation using filled circles */
    int cx = 600, cy = 300;
    gfx_fill_circle(cx, cy, 120, CLR_WIDGET);
    for (int a = 0; a < 360; a += 45) {
        int x1 = cx, y1 = cy;
        int x2 = cx + 118, y2 = cy;
        /* Rotate with simple trig approximation */
        (void)x1; (void)y1; (void)x2; (void)y2;
    }
    /* Simple sector bars */
    for (int i = 0; i < 8; i++) {
        int rx = cx + (i * 17) % 100 - 50;
        int ry = cy + (i * 23) % 100 - 50;
        gfx_fill_circle(rx, ry, 20, seg_colors[i]);
    }

    draw_taskbar(2);
}

int main(void) {
    gfx_clear(CLR_BG);
    draw_desktop();

    /* Simple event loop: keyboard input */
    char buf[16];
    for (;;) {
        long r = read(0, buf, 1);
        if (r <= 0) continue;

        if (buf[0] == 0x1B) break; /* ESC = exit */

        switch (buf[0]) {
            case '1': demo_proc();  break;
            case '2': demo_info();  break;
            case '3': demo_mem();   break;
            case '4': demo_info();  break; /* time/info */
            case '5': demo_gfx();   break;
            case '6': demo_mem();   break;
            case '7': demo_color(); break;
            case '8': demo_color(); break;
            default:
                if (buf[0] != '\n') draw_desktop();
                break;
        }
    }

    gfx_clear(0xFF000000);
    return 0;
}
