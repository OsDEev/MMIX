/* free: show RAM usage. */
#include "libc.h"

int main(void) {
    struct mmix_sysinfo si;
    if (sysinfo(&si) != 0) {
        print("free: sysinfo failed\n");
        return 1;
    }

    uint32_t used = si.total_ram_kb - si.free_ram_kb;

    print("total     used     free\n");
    print_num(si.total_ram_kb / 1024);
    print("MiB    ");
    print_num(used / 1024);
    print("MiB    ");
    print_num(si.free_ram_kb / 1024);
    print("MiB\n");
    return 0;
}
