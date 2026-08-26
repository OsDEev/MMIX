/* busy: spin forever, yielding once per second (Ctrl+C target). */
#include "libc.h"

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    print("busy: spinning, press Ctrl+C to kill me\n");
    for (long i = 0;; i++) {
        if (i % 5000000 == 0) {
            print(".");
            yield();
        }
    }
    return 0;
}
