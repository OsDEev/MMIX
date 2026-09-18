#include "libc.h"

int main(int argc, char **argv) {
    char line[256];
    long n;
    int fd;

    (void)argc;
    (void)argv;

    fd = open("/dev/net");
    if (fd < 0) {
        print("netinfo: /dev/net not available\n");
        return 1;
    }

    if (write(fd, "status", 6) < 0) {
        print("netinfo: command rejected\n");
        close(fd);
        return 1;
    }

    n = read(fd, line, sizeof(line) - 1);
    if (n < 0) {
        print("netinfo: read error\n");
        close(fd);
        return 1;
    }
    if (n == 0) {
        print("netinfo: no data\n");
        close(fd);
        return 1;
    }
    line[n] = 0;
    print(line);

    close(fd);
    return 0;
}
