#include "libc.h"

static int parse_ip(const char *s, unsigned *out) {
    unsigned oct[4];
    unsigned val = 0;
    int idx = 0;
    int digits = 0;
    int i = 0;

    while (s[i]) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            val = val * 10u + (unsigned)(c - '0');
            digits++;
            if (digits > 3) return -1;
            if (val > 255u) return -1;
            i++;
            continue;
        }
        if (c == '.') {
            if (digits == 0 || idx >= 3) return -1;
            oct[idx++] = val;
            val = 0;
            digits = 0;
            i++;
            continue;
        }
        return -1;
    }
    if (idx != 3 || digits == 0) return -1;
    oct[3] = val;
    *out = (oct[0] << 24) | (oct[1] << 16) | (oct[2] << 8) | oct[3];
    return 0;
}

int main(int argc, char **argv) {
    char cmd[24];
    char line[192];
    long n;
    long total;
    int fd;
    int cmd_len;
    unsigned ip = 0;
    const char *arg;

    if (argc < 2) {
        print("usage: ping A.B.C.D\n");
        return 2;
    }
    arg = argv[1];
    if (parse_ip(arg, &ip) != 0) {
        print("ping: invalid address\n");
        return 2;
    }

    fd = open("/dev/net");
    if (fd < 0) {
        print("ping: /dev/net not available\n");
        return 1;
    }

    cmd[0] = 'p'; cmd[1] = 'i'; cmd[2] = 'n'; cmd[3] = 'g'; cmd[4] = ' ';
    {
        unsigned v = ip;
        int pos = 5;
        for (int k = 0; k < 4; k++) {
            unsigned oct = (v >> (24 - 8 * k)) & 0xFFu;
            if (oct == 0) {
                cmd[pos++] = '0';
            } else {
                char rev[4];
                int j = 0;
                while (oct > 0) {
                    rev[j++] = (char)('0' + (oct % 10u));
                    oct /= 10u;
                }
                while (j > 0) cmd[pos++] = rev[--j];
            }
            if (k < 3) cmd[pos++] = '.';
        }
        cmd_len = pos;
    }

    if (write(fd, cmd, (unsigned)cmd_len) < 0) {
        print("ping: command rejected\n");
        close(fd);
        return 1;
    }

    total = 0;
    while (total < (long)sizeof(line) - 1) {
        n = read(fd, line + total, (unsigned)(sizeof(line) - 1 - total));
        if (n < 0) {
            print("ping: read error\n");
            close(fd);
            return 1;
        }
        if (n == 0) break;
        total += n;
    }

    if (total == 0) {
        print("ping: no response from device\n");
        close(fd);
        return 1;
    }
    line[total] = 0;
    print(line);

    close(fd);
    if (line[0] == 'r' && line[1] == 'e' && line[2] == 'p' && line[3] == 'l' &&
        line[4] == 'y') {
        return 0;
    }
    return 1;
}
