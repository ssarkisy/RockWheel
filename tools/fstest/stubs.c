/* What the file system takes from the rest of the firmware. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "host.h"
#include "rtc.h"

volatile long current_tick;

void panicf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "fstest: panic: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    abort();
}

void *mempcpy(void *dst, const void *src, size_t n)
{
    return (char *)memcpy(dst, src, n) + n;
}

int storage_read_sectors(sector_t start, int count, void *buf)
{
    return host_read(start, count, buf);
}

int storage_write_sectors(sector_t start, int count, const void *buf)
{
    return host_write(start, count, buf);
}

int rtc_read_datetime(struct tm *tm)
{
    memset(tm, 0, sizeof *tm);
    tm->tm_year = 126;
    tm->tm_mon = 10;
    tm->tm_mday = 9;
    tm->tm_hour = 12;
    tm->tm_wday = 1;
    return 0;
}

int rtc_write_datetime(const struct tm *tm)
{
    (void)tm;
    return 0;
}

int storage_driver_type(int drive)
{
    (void)drive;
    return STORAGE_ATA_NUM;
}

int find_first_set_bit(uint32_t val)
{
    return val ? __builtin_ctz(val) : 32;
}

/* code pages: names are UTF-8 on the image, no table is ever loaded */
#include "core_alloc.h"
struct buflib_context core_ctx;

int core_alloc_ex(size_t size, struct buflib_callbacks *ops)
{
    (void)size; (void)ops;
    return -1;
}

int core_free(int handle)
{
    (void)handle;
    return 0;
}

void core_pin(int handle)
{
    (void)handle;
}

void core_unpin(int handle)
{
    (void)handle;
}

/* fdprintf() of floating point: no %f is printed here */
#include "ap_int.h"

bool round_number_string10(char *p_rdig, long len)
{
    (void)p_rdig; (void)len;
    abort();
}

char *format_ap_int10(struct ap_int *a, char *p_end)
{
    (void)a; (void)p_end;
    abort();
}

char *format_ap_frac10(struct ap_int *a, char *p_start, long precision)
{
    (void)a; (void)p_start; (void)precision;
    abort();
}
