/*
 * calc: integer expression calculator.
 *
 * usage: calc <expr>     # e.g. calc 2 + 3 * 4
 *        calc            # reads one line from stdin (pipeline-friendly)
 *
 * Operators (loosest to tightest):
 *   |  ^  &  << >>  + -  * / %   unary - ~ +
 * Integer literals: decimal, 0x[hex], 0o[oct], 0b[bin].
 */
#include "libc.h"

static const char *p;

static void skip(void) {
    while (*p == ' ' || *p == '\t') p++;
}

static long expr(void);

static long parse_num(void) {
    skip();
    long v = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') ||
               (*p >= 'A' && *p <= 'F')) {
            char c = *p;
            int d = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : c - 'A' + 10;
            v = v * 16 + d;
            p++;
        }
    } else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
        p += 2;
        while (*p == '0' || *p == '1') { v = v * 2 + (*p - '0'); p++; }
    } else if (p[0] == '0' && (p[1] == 'o' || p[1] == 'O')) {
        p += 2;
        while (*p >= '0' && *p <= '7') { v = v * 8 + (*p - '0'); p++; }
    } else {
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    }
    return v;
}

static long primary(void) {
    skip();
    if (*p == '(') {
        p++;
        long v = expr();
        skip();
        if (*p == ')') p++;
        return v;
    }
    return parse_num();
}

static long unary(void) {
    skip();
    if (*p == '-') { p++; return -unary(); }
    if (*p == '~') { p++; return ~unary(); }
    if (*p == '+') { p++; return unary(); }
    return primary();
}

static void calcfail(const char *msg) {
    print("calc: ");
    print(msg);
    print("\n");
    sys_exit(1);
}

static long mul(void) {
    long v = unary();
    for (;;) {
        skip();
        if (*p == '*') {
            p++;
            v = v * unary();
        } else if (*p == '/') {
            p++;
            long d = unary();
            if (d == 0) calcfail("division by zero");
            v = v / d;
        } else if (*p == '%') {
            p++;
            long d = unary();
            if (d == 0) calcfail("modulo by zero");
            v = v % d;
        } else {
            return v;
        }
    }
}

static long add(void) {
    long v = mul();
    for (;;) {
        skip();
        if (*p == '+') { p++; v = v + mul(); }
        else if (*p == '-') { p++; v = v - mul(); }
        else return v;
    }
}

static long shift(void) {
    long v = add();
    for (;;) {
        skip();
        if (p[0] == '<' && p[1] == '<') { p += 2; v = v << add(); }
        else if (p[0] == '>' && p[1] == '>') { p += 2; v = v >> add(); }
        else return v;
    }
}

static long bit_and(void) {
    long v = shift();
    for (;;) {
        skip();
        if (*p == '&') { p++; v = v & shift(); }
        else return v;
    }
}

static long bit_xor(void) {
    long v = bit_and();
    for (;;) {
        skip();
        if (*p == '^') { p++; v = v ^ bit_and(); }
        else return v;
    }
}

static long expr(void) {
    long v = bit_xor();
    for (;;) {
        skip();
        if (*p == '|') { p++; v = v | bit_xor(); }
        else return v;
    }
}

int main(int argc, char **argv) {
    static char line[264];
    if (argc > 1) {
        size_t i = 0;
        for (int a = 1; a < argc; a++) {
            for (const char *s = argv[a]; *s && i < sizeof(line) - 1; s++)
                line[i++] = *s;
            if (a + 1 < argc && i < sizeof(line) - 1) line[i++] = ' ';
        }
        line[i] = '\0';
        if (i == 0) return 0;
    } else {
        size_t i = 0;
        char c;
        while (read(0, &c, 1) == 1 && c != '\n' && i < sizeof(line) - 1)
            line[i++] = c;
        line[i] = '\0';
    }

    p = line;
    long v = expr();
    print_num(v);
    print("\n");
    return 0;
}