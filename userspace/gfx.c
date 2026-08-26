/* gfx: graphics demo (rectangles, lines, circles) + wait for key. */
#include "libc.h"

#define C_RED    0x00AA0000
#define C_GREEN  0x0000AA00
#define C_BLUE   0x0000AA55
#define C_CYAN   0x0000AAAA
#define C_YELLOW 0x00AAAA00
#define C_WHITE  0x00C8C8C8
#define C_GRAY   0x00555555

int main(void) {
    gfx_fill_rect(0, 0, 1280, 800, 0x00001818);

    for (int i = 0; i < 16; i++) {
        gfx_fill_rect(40 + i * 40, 40, 32, 120,
                      0xFF000000 | ((uint32_t)(i * 16) << 16) |
                      ((uint32_t)(255 - i * 16) << 8));
    }

    gfx_line(40, 260, 660, 420, C_WHITE);
    gfx_line(660, 260, 40, 420, C_CYAN);
    gfx_line(40, 420, 660, 260, C_GREEN);

    gfx_rect(700, 260, 240, 160, C_WHITE);
    gfx_fill_circle(820, 340, 60, C_BLUE);
    gfx_circle(820, 340, 75, C_RED);
    gfx_fill_circle(1000, 340, 40, C_YELLOW);

    gfx_rect(40, 470, 900, 240, C_GRAY);
    for (int i = 0; i < 12; i++) {
        gfx_fill_circle(120 + i * 70, 590, 25,
                        0xFF000000 | ((uint32_t)(i * 20) << 16) |
                        ((uint32_t)(i * 20) << 8) | (uint32_t)(255 - i * 20));
    }

    print("\033[36;1H\033[92;1mMMix graphics demo -- press Enter\033[0m");
    char c;
    read(0, &c, 1);
    gfx_clear(0xFF000000);
    print("\033[2J\033[H");
    return 0;
}
