#include <chardev.h>
#include <kbd.h>
#include <libk.h>
#include <sched.h>
#include <tty.h>
#include <vfs.h>

/*
 * /dev nodes: console (keyboard+framebuffer), null, zero, random.
 */

static uint32_t rng_state = 0x12345678;

static uint32_t rng_next(void) {
    /* xorshift32, re-seeded from the LAPIC tick counter now and then */
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* --- console ------------------------------------------------------------ */

static int console_read(char *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        int c = kbd_getchar();
        if (c < 0) {
            if (got == 0) return -1; /* interrupted */
            break;
        }
        tty_putc((char)c); /* echo */
        buf[got++] = (char)c;
        if (c == '\n') break;
    }
    return (int)got;
}

static int console_write(const char *buf, size_t n) {
    tty_write(buf, n);
    return (int)n;
}

/* --- null / zero / random ------------------------------------------------ */

static int null_read(char *buf, size_t n) {
    (void)buf;
    (void)n;
    return 0; /* EOF */
}

static int null_write(const char *buf, size_t n) {
    (void)buf;
    return (int)n;
}

static int zero_read(char *buf, size_t n) {
    for (size_t i = 0; i < n; i++) buf[i] = 0;
    return (int)n;
}

static int random_read(char *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (i % 4 == 0) rng_state += g_uptime_ticks * 2654435761u;
        buf[i] = (char)(rng_next() >> 24);
    }
    return (int)n;
}

/* --- registry ------------------------------------------------------------ */

static struct chardev dev_console = { "console", console_read, console_write };
static struct chardev dev_null    = { "null",    null_read,    null_write };
static struct chardev dev_zero    = { "zero",    zero_read,    null_write };
static struct chardev dev_random  = { "random",  random_read,  null_write };

void devfs_init(void) {
    vfs_mount_dev("console", &dev_console);
    vfs_mount_dev("null",    &dev_null);
    vfs_mount_dev("zero",    &dev_zero);
    vfs_mount_dev("random",  &dev_random);
    kprintf("[DEV] /dev mounted: console null zero random\n");
}
