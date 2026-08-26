/*
 * MyUnix first user program (/bin/init).
 * Ignores SIGINT, respawns /bin/sh whenever it dies, reaps everything.
 */
#include "libc.h"

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    signal(SIGINT, SIG_IGN);
    setsid();
    setpgid(0, 0);

    char sys[128];
    if (uname(sys) == 0) {
        print("\n=== MMix /init ===\nSystem: ");
        print(sys);
        print("\n\n");
    }

    for (;;) {
        long pid = sys_fork();
        if (pid == 0) {
            char *sh_argv[] = {(char *)"/bin/sh", NULL};
            if (execve("/bin/sh", sh_argv) != 0) {
                print("init: cannot start /bin/sh\n");
                sys_exit(1);
            }
        }

        int status = -1;
        int got = waitpid((int)pid, &status);
        print("[init] shell (pid ");
        print_num(got);
        print(") exited with status ");
        print_num(status);
        print(", restarting\n");
    }
}
