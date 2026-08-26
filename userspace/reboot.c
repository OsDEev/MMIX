/* reboot: reset the machine. */
#include "libc.h"

int main(void) {
    print("Rebooting...\n");
    reboot();
    return 0;
}
