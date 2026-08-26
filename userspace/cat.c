/* cat: dump file contents to stdout. */
#include "libc.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        print("usage: cat <file>\n");
        return 1;
    }

    int fd = open(argv[1]);
    if (fd < 0) {
        print("cat: cannot open ");
        print(argv[1]);
        print("\n");
        return 1;
    }

    char buf[256];
    long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        write(1, buf, (size_t)n);
    }
    close(fd);
    return 0;
}
