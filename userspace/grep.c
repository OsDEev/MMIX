/* grep: print lines matching a pattern (from stdin). */
#include "libc.h"

#define LINE_MAX_LEN 512

int main(int argc, char **argv) {
    if (argc < 2) {
        print("usage: grep <pattern>\n");
        return 1;
    }

    char line[LINE_MAX_LEN];
    size_t n = 0;

    char c;
    while (read(0, &c, 1) == 1) {
        if (c == '\n' || n + 1 >= LINE_MAX_LEN) {
            line[n] = '\0';
            if (strstr(line, argv[1]) != 0) {
                print(line);
                print("\n");
            }
            n = 0;
        } else {
            line[n++] = c;
        }
    }

    if (n > 0) {
        line[n] = '\0';
        if (strstr(line, argv[1]) != 0) {
            print(line);
            print("\n");
        }
    }
    return 0;
}
