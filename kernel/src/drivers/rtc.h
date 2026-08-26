#ifndef MYUNIX_RTC_H
#define MYUNIX_RTC_H

#include <stdint.h>

struct mmix_rtc {
    uint32_t sec, min, hour;
    uint32_t day, mon, year; /* year = full (2026) */
};

void rtc_read(struct mmix_rtc *out);

#endif /* MYUNIX_RTC_H */
