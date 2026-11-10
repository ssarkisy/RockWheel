/* A clock that stays at one moment, so runs write the same bytes. */
#ifndef _RTC_H_
#define _RTC_H_
#include <time.h>
int rtc_read_datetime(struct tm *tm);
int rtc_write_datetime(const struct tm *tm);
#endif
