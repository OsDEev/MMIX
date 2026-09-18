/*
 * echo: print its arguments on one line.
 * (External helper for pipelines; the shell also has echo as a builtin.)
 */
#include "libc.h"

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        print(argv[i]);
        if (i + 1 < argc) print(" ");
    }
    print("\n");
    return 0;
}