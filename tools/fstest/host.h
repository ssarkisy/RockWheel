#ifndef FSTEST_HOST_H
#define FSTEST_HOST_H

#include <stdint.h>

void host_fail(const char *what) __attribute__((noreturn));
void host_open(const char *path);
void host_close(void);
void host_journal(const char *path);
long host_journal_count(void);
int  host_read(uint64_t sector, int count, void *buf);
int  host_write(uint64_t sector, int count, const void *buf);
void host_clone(const char *src, const char *dst);
long host_apply(const char *journal_path, const char *img, long k);

#endif
