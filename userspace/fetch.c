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
    const char *C_LOGO = "\033[1;36m";
    const char *C_LABEL = "\033[1;92m";
    const char *C_RESET = "\033[0m";

    char line[128];
    int li = 0;

#define NEXTLINE()                                          \
    do {                                                    \
        print(C_LOGO);                                      \
        print(logo[li < logo_n ? li : logo_n - 1]);         \
        print(C_RESET);                                     \
        print(line);                                        \
        print("\n");                                        \
        li++;                                               \
    } while (0)

    /* header */
    memcpy(line, "\x1b", 0); /* no-op keep compilers quiet */
    {
        const char *host = "\033[1;92mmix\033[1;35m@\033[1;92mmmix\033[0m";
        int i = 0;
        for (; host[i]; i++) line[i] = host[i];
        line[i] = '\0';
    }
    NEXTLINE();
    {
        const char *bar = "\033[90m--------------------\033[0m";
        int i = 0;
        for (; bar[i]; i++) line[i] = bar[i];
        line[i] = '\0';
    }
    NEXTLINE();

    /* OS */
    memcpy(line, "\033[1;92mOS:\033[0m ", 14);
    {
        int i = 4;
        const char *s = os;
        while (*s) line[i++] = *s++;
        line[i] = '\0';
    }
    NEXTLINE();

    /* Uptime */
    if (have_si) {
        memcpy(line, "\033[1;92mUptime:\033[0m ", 21);
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
        memcpy(line, "\033[1;92mShell:\033[0m /bin/sh", 26);
        NEXTLINE();
    }

    /* Resolution */
    if (have_si && si.fb_w) {
        memcpy(line, "\033[1;92mResolution:\033[0m ", 26);
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
        memcpy(line, "\033[1;92mRAM:\033[0m ", 17);
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
        const char *blocks = "\033[41m   \033[42m   \033[43m   \033[44m   \033[45m   \033[46m   \033[0m";
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
