/*
 * base64: encode (default) or decode (-d) stdin to stdout.
 * Encoding wraps output at 76 columns; decoding skips whitespace.
 */
#include "libc.h"

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void emit(const char *s, size_t n) {
    write(1, s, n);
}

static void encode_stream(void) {
    unsigned char in[3];
    int inlen = 0;
    int col = 0;
    char out[4];
    char c;

    while (read(0, &c, 1) == 1) {
        in[inlen++] = (unsigned char)c;
        if (inlen != 3) continue;

        out[0] = b64_table[in[0] >> 2];
        out[1] = b64_table[((in[0] & 0x03) << 4) | (in[1] >> 4)];
        out[2] = b64_table[((in[1] & 0x0F) << 2) | (in[2] >> 6)];
        out[3] = b64_table[in[2] & 0x3F];
        emit(out, 4);

        col += 4;
        if (col >= 76) { emit("\n", 1); col = 0; }
        inlen = 0;
    }

    if (inlen == 1) {
        out[0] = b64_table[in[0] >> 2];
        out[1] = b64_table[(in[0] & 0x03) << 4];
        out[2] = '=';
        out[3] = '=';
        emit(out, 4);
        emit("\n", 1);
    } else if (inlen == 2) {
        out[0] = b64_table[in[0] >> 2];
        out[1] = b64_table[((in[0] & 0x03) << 4) | (in[1] >> 4)];
        out[2] = b64_table[(in[1] & 0x0F) << 2];
        out[3] = '=';
        emit(out, 4);
        emit("\n", 1);
    } else if (col != 0) {
        emit("\n", 1);
    }
}

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static void decode_stream(void) {
    int vals[4];
    int n = 0;
    char out[3];
    char c;

    while (read(0, &c, 1) == 1) {
        int v = b64_val(c);
        if (v < 0) continue; /* skip whitespace and padding */
        vals[n++] = v;
        if (n != 4) continue;

        long acc = (vals[0] << 18) | (vals[1] << 12) | (vals[2] << 6) | vals[3];
        out[0] = (char)(acc >> 16);
        out[1] = (char)(acc >> 8);
        out[2] = (char)(acc);
        emit(out, 3);
        n = 0;
    }

    if (n == 2) {
        long acc = (vals[0] << 18) | (vals[1] << 12);
        out[0] = (char)(acc >> 16);
        emit(out, 1);
    } else if (n == 3) {
        long acc = (vals[0] << 18) | (vals[1] << 12) | (vals[2] << 6);
        out[0] = (char)(acc >> 16);
        out[1] = (char)(acc >> 8);
        emit(out, 2);
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        decode_stream();
    } else {
        encode_stream();
    }
    return 0;
}