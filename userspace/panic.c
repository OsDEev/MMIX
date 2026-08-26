/* panic: request a kernel panic through rudod (root user do). */
#include "libc.h"

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    print("\033[1;91mpanic:\033[0m requesting kernel panic via rudo...\n");

    if (rudo_request(RUDO_OP_PANIC) != 0) {
        print("panic: rudo denied (rudod not running?)\n");
        return 1;
    }

    print("panic: approved. goodbye\n");
    yield();
    return 0;
}
