/* fetch: MMix system information (fastfetch-style). */
#include "libc.h"

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    struct mmix_sysinfo si;
    int have_si = (sysinfo(&si) == 0);

    char sys[128];
    const char *os = "MMix";
    if (uname(sys) == 0) os = sys;

    static const char *logo[] = {
        "  /\\\\    ",
        " /  \\\\   ",
        "/ /\\ \\\\  ",
        "\\ \\\\/ // ",
        " \\// //  ",
        "  / //   ",
        " / //    ",
        "/_//     "
    };
    int logo_n = 8;

    char line[128];
    int li = 0;

#define NEXTLINE()                                     \
    do {                                               \
        print(logo[li < logo_n ? li : logo_n - 1]);    \
        print(line);                                   \
        print("\n");                                   \
        li++;                                          \
    } while (0)

    /* header */
    memcpy(line, "\x1b", 0); /* no-op keep compilers quiet */
    {
        char host[64];
        memcpy(host, "mmix@mmix", 10);
        int i = 0;
        for (; host[i]; i++) line[i] = host[i];
        line[i] = '\0';
    }
    NEXTLINE();
    {
        const char *bar = "---------";
        int i = 0;
        for (; bar[i]; i++) line[i] = bar[i];
        line[i] = '\0';
    }
    NEXTLINE();

    /* OS */
    memcpy(line, "OS: ", 5);
    {
        int i = 4;
        const char *s = os;
        while (*s) line[i++] = *s++;
        line[i] = '\0';
    }
    NEXTLINE();

    /* Uptime */
    if (have_si) {
        memcpy(line, "Uptime: ", 9);
        int i = 8;
        uint32_t m = si.uptime_s / 60, s = si.uptime_s % 60;
        if (m) {
            if (m >= 10) line[i++] = (char)('0' + m / 10);
            line[i++] = (char)('0' + m % 10);
            line[i++] = 'm';
        }
        if (s >= 10) line[i++] = (char)('0' + s / 10);
        line[i++] = (char)('0' + s % 10);
        line[i++] = 's';
        line[i] = '\0';
        NEXTLINE();
    }

    /* Shell */
    {
        memcpy(line, "Shell: /bin/sh", 15);
        NEXTLINE();
    }

    /* Resolution */
    if (have_si && si.fb_w) {
        memcpy(line, "Resolution: ", 13);
        int i = 12;
        uint32_t v = si.fb_w;
        char tmp[12];
        int t = 0;
        while (v) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
        while (t) line[i++] = tmp[--t];
        line[i++] = 'x';
        v = si.fb_h; t = 0;
        while (v) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
        while (t) line[i++] = tmp[--t];
        line[i++] = '@';
        v = si.fb_bpp; t = 0;
        while (v) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
        while (t) line[i++] = tmp[--t];
        line[i] = '\0';
        NEXTLINE();
    }

    /* RAM */
    if (have_si) {
        memcpy(line, "RAM: ", 6);
        int i = 5;
        uint32_t used_kb = si.total_ram_kb - si.free_ram_kb;
        uint32_t v = used_kb / 1024;
        char tmp[12];
        int t = 0;
        while (v) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
        while (t) line[i++] = tmp[--t];
        line[i++] = 'M';
        line[i++] = 'i';
        line[i++] = 'B';
        line[i++] = ' ';
        line[i++] = '/';
        line[i++] = ' ';
        v = si.total_ram_kb / 1024; t = 0;
        while (v) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
        while (t) line[i++] = tmp[--t];
        line[i++] = 'M';
        line[i++] = 'i';
        line[i++] = 'B';
        line[i] = '\0';
        NEXTLINE();
    }

    /* color blocks */
    {
        const char *blocks = "##### ##### ##### ##### ##### #####";
        int i = 0;
        for (; blocks[i]; i++) line[i] = blocks[i];
        line[i] = '\0';
        NEXTLINE();
    }

    while (li < logo_n) {
        print(logo[li]);
        print("\n");
        li++;
    }

    return 0;
}
