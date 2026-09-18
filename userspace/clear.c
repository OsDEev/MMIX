/*
 * clear: clear the terminal screen (ANSI 2J + home).
 */
#include "libc.h"

int main(void) {
    print("\033[2J\033[H");
    return 0;
}