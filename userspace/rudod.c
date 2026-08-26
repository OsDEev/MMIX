/*
 * rudod -- root user do (MMIX root daemon, uid 0).
 *
 * Parks in rudo_wait() until an unprivileged process requests a
 * privileged operation. PANIC requests trigger an intentional kernel
 * page fault. SETUID requests are auto-approved (kernel sets the
 * requester's uid to 0).
 */
#include "libc.h"

int main(void) {
    print("\033[1;92mrudod\033[0m: root daemon online (uid 0)\n");

    for (;;) {
        int req[2] = {0, 0};
        if (rudo_wait(req) < 0) {
            print("rudod: wait failed\n");
            yield();
            continue;
        }

        if (req[0] == RUDO_OP_PANIC) {
            print("rudod: approved PANIC from pid ");
            print_num(req[1]);
            print("\n");
        } else if (req[0] == RUDO_OP_SETUID) {
            print("rudod: approved SETUID for pid ");
            print_num(req[1]);
            print("\n");
        } else {
            print("rudod: unknown op ");
            print_num(req[0]);
            print("\n");
        }
    }
}
