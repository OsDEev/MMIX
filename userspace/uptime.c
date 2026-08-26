/* uptime: time since boot (reads /proc/uptime). */
#include "libc.h"

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    int fd = open("/proc/uptime");
    if (fd < 0) {
        print("uptime: cannot open /proc/uptime\n");
        return 1;
    }

    char buf[64];
    long n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0) n = 0;
    buf[n] = '\0';

    print("up ");
    print(buf);
    return 0;
}
