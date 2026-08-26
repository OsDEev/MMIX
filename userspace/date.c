/* date: current time from the CMOS RTC. */
#include "libc.h"

int main(void) {
    struct mmix_timeval tv;
    if (systime(&tv) != 0) {
        print("date: failed\n");
        return 1;
    }

    char buf[32];
    int i = 0;
#define P2(v) buf[i++] = (char)('0' + (v) / 10); buf[i++] = (char)('0' + (v) % 10)
    P2(tv.day); buf[i++] = '.'; P2(tv.mon); buf[i++] = '.';
    buf[i++] = '2'; buf[i++] = '0'; P2(tv.year % 100); buf[i++] = ' ';
    P2(tv.hour); buf[i++] = ':'; P2(tv.min); buf[i++] = ':'; P2(tv.sec);
#undef P2
    buf[i] = '\0';
    print(buf);
    print("\n");
    return 0;
}
