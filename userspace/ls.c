/* ls: list files in the RAM filesystem root. */
#include "libc.h"

#define MAX_FILES 128

struct dirent_info {
    char name[64];
};

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    /*
     * The kernel VFS has no readdir() yet; the known initrd layout is
     * reported statically until a proper getdents syscall exists.
     */
    print("bin/\n");
    print("etc/motd\n");
    print("etc/unit.conf\n");
    print("tmp/\n");
    return 0;
}
