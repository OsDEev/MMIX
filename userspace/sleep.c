/* sleep N: wait N seconds. */
#include "libc.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        print("usage: sleep <seconds>\n");
        return 1;
    }

    int want = 0;
    for (const char *p = argv[1]; *p; p++) {
        if (*p < '0' || *p > '9') break;
        want = want * 10 + (*p - '0');
    }

    struct mmix_sysinfo si;
    sysinfo(&si);
    uint32_t until = si.uptime_s + (uint32_t)want;

    while (si.uptime_s < until) {
        yield();
        sysinfo(&si);
    }
    return 0;
}
