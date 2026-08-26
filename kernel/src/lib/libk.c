#include <libk.h>
#include <io.h>

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

static void serial_putc(char c) {
    if (c == '\n') {
        outb(COM1, '\r');
    }
    outb(COM1, (uint8_t)c);
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
        switch (*fmt) {
            case 's': {
                const char *s = va_arg(args, const char *);
                kprint(s ? s : "(null)");
                break;
            }
            case 'd':
                print_int(va_arg(args, int));
                break;
            case 'u':
                print_uint(va_arg(args, uint64_t), 10, false);
                break;
            case 'x':
                print_uint(va_arg(args, uint64_t), 16, false);
                break;
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

void vsnprintf_mini(char *out, size_t cap, const char *fmt, va_list ap) {
    size_t o = 0;
    if (cap == 0) return;

    auto void put(char c) { if (o + 1 < cap) out[o++] = c; }

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { put(*p); continue; }
        p++;

        int longmod = 0;
        while (*p == 'l') { longmod = 1; p++; }

        switch (*p) {
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (s == NULL) s = "(null)";
                while (*s) put(*s++);
                break;
            }
            case 'd': {
                long v = longmod ? va_arg(ap, long) : va_arg(ap, int);
                char num[24];
                int i = 0;
                if (v < 0) { put('-'); v = -v; }
                do { num[i++] = (char)('0' + v % 10); v /= 10; } while (v && i < 24);
                while (i) put(num[--i]);
                break;
            }
            case 'u': {
                unsigned long v = longmod ? va_arg(ap, unsigned long)
                                          : va_arg(ap, unsigned int);
                char num[24];
                int i = 0;
                do { num[i++] = (char)('0' + v % 10); v /= 10; } while (v && i < 24);
                while (i) put(num[--i]);
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
                while (i) put(num[--i]);
                break;
            }
            case 'c':
                put((char)va_arg(ap, int));
                break;
            case '%':
                put('%');
                break;
            default:
                put('%');
                put(*p);
                break;
        }
    }
    out[o] = '\0';
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
