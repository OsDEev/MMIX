#include <gfx.h>
#include <libk.h>
#include <panic.h>
#include <sched.h>
#include <tty.h>

#define PANIC_BG 0xFF0078D7

void panic(const char *fmt, ...) {
    __asm__ volatile("cli");

    char reason[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf_mini(reason, sizeof(reason) - 1, fmt, ap);
    va_end(ap);
    reason[sizeof(reason) - 1] = '\0';

    gfx_clear(PANIC_BG);
    tty_set_colors(0xFFFFFFFF, PANIC_BG);

    task_t *cur = sched_get_current();

    tty_printf("\033[H");
    tty_printf("\n");
    tty_printf("    :(\n");
    tty_printf("\n");
    tty_printf("    \033[1;97mMMIX KERNEL PANIC\033[39m\n");
    tty_printf("\n");
    tty_printf("    \033[1;93m%s\033[39m\n", reason);
    tty_printf("\n");
    if (cur != NULL) {
        tty_printf("    Task:    %s (PID %d, PGID %d)\n",
                cur->name, cur->pid, cur->pgid);
    } else {
        tty_printf("    Task:    <none>\n");
    }
    tty_printf("    Uptime:  %u s\n",
            (uint32_t)(g_uptime_ticks / 50));
    tty_printf("\n");
    tty_printf("    The system has been halted.\n");
    tty_printf("    Reset the machine to recover.\n");

    for (;;) {
        __asm__ volatile("hlt");
    }
}
