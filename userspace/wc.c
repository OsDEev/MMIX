/* wc: count lines, words, bytes from stdin. */
#include "libc.h"

int main(void) {
    long lines = 0, words = 0, bytes = 0;
    int in_word = 0;

    char c;
    while (read(0, &c, 1) == 1) {
        bytes++;
        if (c == '\n') lines++;
        if (c == ' ' || c == '\n' || c == '\t') {
            in_word = 0;
        } else if (!in_word) {
            in_word = 1;
            words++;
        }
    }

    print_num(lines);
    print(" ");
    print_num(words);
    print(" ");
    print_num(bytes);
    print("\n");
    return 0;
}
