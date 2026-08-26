/* ps: list processes (reads /proc/stat). */
#include "libc.h"

#define BUF_SIZE 2048

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    int fd = open("/proc/stat");
    if (fd < 0) {
        print("ps: cannot open /proc/stat\n");
        return 1;
    }

    static char buf[BUF_SIZE];
    long n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0) n = 0;
    buf[n] = '\0';

    print("PID  PPID  PGID  S  NAME\n");
    print(buf);
    return 0;
}
