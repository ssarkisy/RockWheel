#pragma once

#ifndef ARRAYLEN
#define ARRAYLEN(x) (sizeof(x) / sizeof((x)[0]))
#endif

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef ROCKBOX_DIR
#define ROCKBOX_DIR "/.rockbox"
#endif

#ifndef O_RDONLY
#define O_RDONLY 0
#endif

static inline int open_utf8(const char* path, int flags) {
    (void)path;
    (void)flags;
    return -1;
}

static inline int read_line(int fd, char* buf, int size) {
    (void)fd;
    if(size > 0) {
        buf[0] = '\0';
    }
    return 0;
}

static inline int close(int fd) {
    (void)fd;
    return 0;
}
