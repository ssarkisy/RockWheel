/* The image and the journal on the host's own files: built without the
   Rockbox headers, whose open() and read() are the file system under test. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <sys/clonefile.h>
#endif
#include "host.h"

#define SECTOR 512

static int img_fd = -1;
static uint64_t img_sectors;
static FILE *journal;
static long journal_count;

void host_fail(const char *what)
{
    fprintf(stderr, "fstest: %s: %s\n", what, strerror(errno));
    exit(2);
}

void host_open(const char *path)
{
    img_fd = open(path, O_RDWR);
    if (img_fd < 0)
        host_fail(path);
    struct stat st;
    if (fstat(img_fd, &st) < 0)
        host_fail(path);
    img_sectors = st.st_size / SECTOR;
}

void host_close(void)
{
    if (journal && fclose(journal) != 0)
        host_fail("journal");
    journal = NULL;
    if (img_fd >= 0)
        close(img_fd);
    img_fd = -1;
}

/* every sector written from now on goes to the journal too, one record
   each: its number and its bytes, so a run can be cut between any two */
void host_journal(const char *path)
{
    journal = fopen(path, "wb");
    if (!journal)
        host_fail(path);
    journal_count = 0;
}

long host_journal_count(void)
{
    return journal_count;
}

int host_read(uint64_t sector, int count, void *buf)
{
    if (sector + count > img_sectors)
        return -1;
    ssize_t n = pread(img_fd, buf, (size_t)count * SECTOR, sector * SECTOR);
    return n == (ssize_t)count * SECTOR ? 0 : -2;
}

int host_write(uint64_t sector, int count, const void *buf)
{
    if (sector + count > img_sectors)
        return -1;
    ssize_t n = pwrite(img_fd, buf, (size_t)count * SECTOR, sector * SECTOR);
    if (n != (ssize_t)count * SECTOR)
        return -2;
    for (int i = 0; journal && i < count; i++)
    {
        uint64_t s = sector + i;
        if (fwrite(&s, sizeof s, 1, journal) != 1 ||
            fwrite((const char *)buf + i * SECTOR, SECTOR, 1, journal) != 1)
            host_fail("journal");
        journal_count++;
    }
    return 0;
}

void host_clone(const char *src, const char *dst)
{
    unlink(dst);
#ifdef __APPLE__
    if (clonefile(src, dst, 0) == 0)
        return;
#endif
    FILE *in = fopen(src, "rb"), *out = fopen(dst, "wb");
    if (!in || !out)
        host_fail(dst);
    static char buf[1 << 16];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n)
            host_fail(dst);
    fclose(in);
    if (fclose(out) != 0)
        host_fail(dst);
}

/* the image as the disk holds it after the first k writes of the journal */
long host_apply(const char *journal_path, const char *img, long k)
{
    FILE *j = fopen(journal_path, "rb");
    if (!j)
        host_fail(journal_path);
    int fd = open(img, O_RDWR);
    if (fd < 0)
        host_fail(img);
    long done = 0;
    uint64_t s;
    char buf[SECTOR];
    while (done < k && fread(&s, sizeof s, 1, j) == 1)
    {
        if (fread(buf, SECTOR, 1, j) != 1)
            break;
        if (pwrite(fd, buf, SECTOR, s * SECTOR) != SECTOR)
            host_fail(img);
        done++;
    }
    fclose(j);
    close(fd);
    return done;
}
