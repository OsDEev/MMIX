#include <libk.h>
#include <io.h>
#include <tty.h>

/* COM1 */
#define COM1 0x3F8

void serial_init(void) {
    outb(COM1 + 1, 0x00); /* Disable interrupts */
    outb(COM1 + 3, 0x80); /* Enable DLAB */
    outb(COM1 + 0, 0x01); /* Divisor low (115200 baud) */
    outb(COM1 + 1, 0x00); /* Divisor high */
    outb(COM1 + 3, 0x03); /* 8 bits, no parity, one stop bit */
    outb(COM1 + 2, 0xC7); /* Enable FIFO, clear, 14-byte threshold */
    outb(COM1 + 4, 0x0B); /* IRQs enabled, RTS/DSR set */
}

static volatile int g_serial_flood = 0;
static char g_last_ch = 0;
static int g_repeat = 0;

/* Burst tracing: capture the first 32 bytes of each new output burst. */
#define BURST_CAP 32
static char  g_burst_buf[BURST_CAP];
static int   g_burst_len = 0;
static int   g_burst_total = 0;  /* total chars in current burst */
static int   g_burst_id = 0;     /* burst sequence number */

static void flush_burst(void) {
    if (g_burst_len == 0 && g_burst_total == 0) return;
    /* Print captured header */
    const char hdr[] = "[BURST#";
    for (int i = 0; hdr[i]; i++) outb(COM1, (uint8_t)hdr[i]);
    /* print burst id (decimal) */
    static const char dx[] = "0123456789";
    int v = g_burst_id;
    char dbuf[12];
    int dn = 0;
    if (v == 0) { dbuf[dn++] = '0'; }
    else { while (v > 0) { dbuf[dn++] = dx[v % 10]; v /= 10; } }
    for (int i = dn - 1; i >= 0; i--) outb(COM1, (uint8_t)dbuf[i]);
    const char mid[] = "] len=";
    for (int i = 0; mid[i]; i++) outb(COM1, (uint8_t)mid[i]);
    /* print total len (decimal) */
    v = g_burst_total;
    dn = 0;
    if (v == 0) { dbuf[dn++] = '0'; }
    else { while (v > 0) { dbuf[dn++] = dx[v % 10]; v /= 10; } }
    for (int i = dn - 1; i >= 0; i--) outb(COM1, (uint8_t)dbuf[i]);
    const char mid2[] = " first=";
    for (int i = 0; mid2[i]; i++) outb(COM1, (uint8_t)mid2[i]);
    /* print captured bytes as hex */
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < g_burst_len; i++) {
        outb(COM1, (uint8_t)hx[((uint8_t)g_burst_buf[i]) >> 4]);
        outb(COM1, (uint8_t)hx[((uint8_t)g_burst_buf[i]) & 0xF]);
        outb(COM1, ' ');
    }
    outb(COM1, '\r');
    outb(COM1, '\n');
    g_burst_id++;
    g_burst_len = 0;
    g_burst_total = 0;
}

static void serial_putc(char c) {
    if (c == '\n') {
        outb(COM1, '\r');
        g_repeat = 0;
        g_burst_len = 0;
        g_burst_total = 0;
    }
    if (!g_serial_flood) {
        /* Capture first bytes of each burst */
        if (g_burst_len < BURST_CAP) {
            g_burst_buf[g_burst_len++] = c;
        }
        g_burst_total++;
        if (c == g_last_ch && c != '\n' && c != '\r') {
            if (++g_repeat > 128) {
                g_serial_flood = 1;
                flush_burst();
                const char msg[] = "\n[FLOOD] serial flood detected: repeating char 0x";
                for (int i = 0; msg[i]; i++) outb(COM1, (uint8_t)msg[i]);
                static const char hx[] = "0123456789abcdef";
                outb(COM1, (uint8_t)hx[((uint8_t)c) >> 4]);
                outb(COM1, (uint8_t)hx[((uint8_t)c) & 0xF]);
                outb(COM1, '\r');
                outb(COM1, '\n');
                return;
            }
        } else {
            g_repeat = 0;
        }
        g_last_ch = c;
    }
    if (g_serial_flood) {
        if (c == '\n') g_serial_flood = 0;
        else return;
    }
    outb(COM1, (uint8_t)c);
    tty_screen_putc(c);
}

void kprint(const char *str) {
    while (*str) {
        serial_putc(*str++);
    }
}

static void print_uint(uint64_t val, int base, bool uppercase) {
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[65];
    int i = 0;

    if (val == 0) {
        buf[i++] = '0';
    } else {
        while (val > 0) {
            buf[i++] = digits[val % base];
            val /= base;
        }
    }

    for (int j = i - 1; j >= 0; j--) {
        serial_putc(buf[j]);
    }
}

static void print_int(int64_t val) {
    if (val < 0) {
        serial_putc('-');
        print_uint((uint64_t)(-val), 10, false);
    } else {
        print_uint((uint64_t)val, 10, false);
    }
}

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    while (*fmt) {
        if (*fmt != '%') {
            serial_putc(*fmt++);
            continue;
        }

        fmt++;

        /* Parse optional zero-padding width: %02x, %04x, etc. */
        int pad_width = 0;
        char pad_char = ' ';
        if (*fmt == '0') {
            pad_char = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            pad_width = pad_width * 10 + (*fmt - '0');
            fmt++;
        }

        /* Handle %lx, %ld, %lu, %lx */
        if (*fmt == 'l') {
            fmt++;
            switch (*fmt) {
                case 'u': print_uint(va_arg(args, unsigned long), 10, false); break;
                case 'd': print_int((int64_t)va_arg(args, long)); break;
                case 'x': print_uint(va_arg(args, unsigned long), 16, false); break;
                case 'p': {
                    uint64_t v = (uint64_t)va_arg(args, void *);
                    kprint("0x");
                    print_uint(v, 16, false);
                    break;
                }
                default:
                    serial_putc('l');
                    serial_putc(*fmt);
                    break;
            }
            fmt++;
            continue;
        }

        switch (*fmt) {
            case 's': {
                const char *s = va_arg(args, const char *);
                kprint(s ? s : "(null)");
                break;
            }
            case 'd': {
                int v = va_arg(args, int);
                /* For padded %0Nd: unsigned hex-like, but we want decimal padding */
                if (pad_width > 0 && v >= 0) {
                    /* Print with zero-padding for decimal */
                    char num[24];
                    int i = 0;
                    unsigned uv = (unsigned)v;
                    if (uv == 0) num[i++] = '0';
                    else { while (uv) { num[i++] = '0' + uv % 10; uv /= 10; } }
                    while (i < pad_width) { serial_putc('0'); i++; }
                    for (int j = i - 1; j >= 0; j--) serial_putc(num[j]);
                } else {
                    print_int(v);
                }
                break;
            }
            case 'u': {
                uint64_t v = va_arg(args, uint64_t);
                if (pad_width > 0) {
                    char num[24];
                    int i = 0;
                    if (v == 0) num[i++] = '0';
                    else { while (v) { num[i++] = '0' + v % 10; v /= 10; } }
                    int actual = i;
                    while (actual < pad_width) { serial_putc(pad_char); actual++; }
                    for (int j = i - 1; j >= 0; j--) serial_putc(num[j]);
                } else {
                    print_uint(v, 10, false);
                }
                break;
            }
            case 'x': {
                uint64_t v = va_arg(args, uint64_t);
                if (pad_width > 0) {
                    char num[24];
                    int i = 0;
                    if (v == 0) num[i++] = '0';
                    else { while (v) { num[i++] = "0123456789abcdef"[v & 0xF]; v >>= 4; } }
                    int actual = i;
                    while (actual < pad_width) { serial_putc(pad_char); actual++; }
                    for (int j = i - 1; j >= 0; j--) serial_putc(num[j]);
                } else {
                    print_uint(v, 16, false);
                }
                break;
            }
            case 'X':
                print_uint(va_arg(args, uint64_t), 16, true);
                break;
            case 'p': {
                uint64_t val = (uint64_t)va_arg(args, void *);
                kprint("0x");
                print_uint(val, 16, false);
                break;
            }
            case 'c':
                serial_putc((char)va_arg(args, int));
                break;
            case '%':
                serial_putc('%');
                break;
            default:
                serial_putc('%');
                serial_putc(*fmt);
                break;
        }
        fmt++;
    }

    va_end(args);
}

/* Output sink for vsnprintf_mini (a plain struct, clang-compatible). */
struct fmt_buf {
    char *out;
    size_t cap;
    size_t o;
};

static void fmt_put(struct fmt_buf *b, char c) {
    if (b->o + 1 < b->cap) b->out[b->o++] = c;
}

void vsnprintf_mini(char *out, size_t cap, const char *fmt, va_list ap) {
    if (cap == 0) return;

    struct fmt_buf b;
    b.out = out;
    b.cap = cap;
    b.o = 0;

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { fmt_put(&b, *p); continue; }
        p++;

        int longmod = 0;
        while (*p == 'l') { longmod = 1; p++; }

        switch (*p) {
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (s == NULL) s = "(null)";
                while (*s) fmt_put(&b, *s++);
                break;
            }
            case 'd': {
                long v = longmod ? va_arg(ap, long) : va_arg(ap, int);
                char num[24];
                int i = 0;
                if (v < 0) { fmt_put(&b, '-'); v = -v; }
                do { num[i++] = (char)('0' + v % 10); v /= 10; } while (v && i < 24);
                while (i) fmt_put(&b, num[--i]);
                break;
            }
            case 'u': {
                unsigned long v = longmod ? va_arg(ap, unsigned long)
                                          : va_arg(ap, unsigned int);
                char num[24];
                int i = 0;
                do { num[i++] = (char)('0' + v % 10); v /= 10; } while (v && i < 24);
                while (i) fmt_put(&b, num[--i]);
                break;
            }
            case 'x': {
                unsigned long v = longmod ? va_arg(ap, unsigned long)
                                          : va_arg(ap, unsigned int);
                char num[24];
                int i = 0;
                do {
                    num[i++] = "0123456789abcdef"[v & 0xF];
                    v >>= 4;
                } while (v && i < 24);
                while (i) fmt_put(&b, num[--i]);
                break;
            }
            case 'c':
                fmt_put(&b, (char)va_arg(ap, int));
                break;
            case '%':
                fmt_put(&b, '%');
                break;
            default:
                fmt_put(&b, '%');
                fmt_put(&b, *p);
                break;
        }
    }
    b.out[b.o] = '\0';
}

size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

/* Minimal snprintf into a fixed buffer (kernel utility). */
int snprintf(char *out, size_t cap, const char *fmt, ...) {
    if (cap == 0) return 0;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf_mini(out, cap, fmt, ap);
    va_end(ap);
    return strlen(out);
}
