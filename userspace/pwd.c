/*
 * pwd: print the working directory.
 * The VFS is a single tree rooted at / and the shell always runs
 * from the root, so the answer is always "/".
 */
#include "libc.h"

int main(void) {
    print("/\n");
    return 0;
}