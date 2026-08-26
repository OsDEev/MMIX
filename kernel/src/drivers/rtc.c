#include <io.h>
#include <rtc.h>

/* CMOS RTC via ports 0x70/0x71. */

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

#define RTC_SEC  0x00
#define RTC_MIN  0x02
#define RTC_HOUR 0x04
#define RTC_DAY  0x07
#define RTC_MON  0x08
#define RTC_YEAR 0x09
#define RTC_A    0x0A
#define RTC_B    0x0B

static uint8_t cmos(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static int updating(void) {
    return cmos(RTC_A) & 0x80;
}

static uint8_t to_binary(uint8_t v, uint8_t regb) {
    if (regb & 0x04) return v;             /* binary mode */
    return (uint8_t)((v & 0x0F) + (v >> 4) * 10);
}

void rtc_read(struct mmix_rtc *out) {
    while (updating()) { }

    uint8_t sec  = cmos(RTC_SEC);
    uint8_t min  = cmos(RTC_MIN);
    uint8_t hour = cmos(RTC_HOUR);
    uint8_t day  = cmos(RTC_DAY);
    uint8_t mon  = cmos(RTC_MON);
    uint8_t year = cmos(RTC_YEAR);

    uint8_t regb = cmos(RTC_B);

    /* Re-read if the century rolled over mid-read */
    while (updating()) { }
    if (sec != cmos(RTC_SEC)) {
        sec  = cmos(RTC_SEC);
        min  = cmos(RTC_MIN);
        hour = cmos(RTC_HOUR);
        day  = cmos(RTC_DAY);
        mon  = cmos(RTC_MON);
        year = cmos(RTC_YEAR);
    }

    out->sec  = to_binary(sec, regb);
    out->min  = to_binary(min, regb);
    out->hour = to_binary(hour, regb);
    out->day  = to_binary(day, regb);
    out->mon  = to_binary(mon, regb);
    out->year = (uint32_t)to_binary(year, regb) + 2000;
}
