/* Included before anything else in each file system source built on the host:
   it stands in for config.h with an iPod Video's storage options, and takes
   the include guards of the target headers the stubs replace. */
#ifndef FSTEST_CONFIG_H
#define FSTEST_CONFIG_H

#define __CONFIG_H__
#define __STORAGE_H__
#define DEBUG_H
#define __PANIC_H__
#define LOGF_H

#define PLATFORM_NATIVE       (1<<0)
#define PLATFORM_HOSTED       (1<<1)
#define PLATFORM_ANDROID      (1<<2)
#define PLATFORM_SDL          (1<<3)
#define PLATFORM_GAME_CONSOLE (1<<4)
#define CONFIG_PLATFORM       PLATFORM_NATIVE

#define STORAGE_ATA_NUM   0
#define STORAGE_NUM_TYPES 7
#define STORAGE_ATA     (1 << 0)
#define STORAGE_MMC     (1 << 1)
#define STORAGE_SD      (1 << 2)
#define STORAGE_NAND    (1 << 3)
#define STORAGE_RAMDISK (1 << 4)
#define STORAGE_USB     (1 << 5)
#define STORAGE_HOSTFS  (1 << 6)
#define CONFIG_STORAGE  STORAGE_ATA

#define HAVE_LBA48
#define HAVE_MULTIVOLUME
#define NUM_DRIVES            1
#define NUM_VOLUMES_PER_DRIVE 4
#define NUM_VOLUMES           (NUM_DRIVES * NUM_VOLUMES_PER_DRIVE)
#define NUM_CORES             1
#define CONFIG_CPU            0
#define ROCKBOX_LITTLE_ENDIAN 1
#define HZ                    100
#define CONFIG_RTC            1

#ifndef MEMORYSIZE
#define MEMORYSIZE 32 /* the owner's iPod; the Makefile may say less */
#endif
#define UNICODE32
#define ucschar_t unsigned int

#define ROCKBOX_DIR "/.rockbox"

/* lets fat.c mount a FAT32 volume smaller than the spec's minimum */
#define TEST_FAT

#define IBSS_ATTR
#define IDATA_ATTR
#define ICODE_ATTR
#define SHAREDBSS_ATTR
#define SHAREDDATA_ATTR
#define INIT_ATTR
#define NO_INLINE __attribute__((noinline))

#define IF_COP(...)
#define IF_COP_VOID(...) void
#define IF_PRIO(...)

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <limits.h>

/* debug.h, panic.h, logf.h */
#define DEBUGF(...)  do { } while (0)
#define LDEBUGF(...) do { } while (0)
#define logf(...)    do { } while (0)
void panicf(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

/* storage.h: the sectors of the image (host.c) */
#include "mv.h"
int storage_read_sectors(sector_t start, int count, void *buf);
int storage_write_sectors(sector_t start, int count, const void *buf);
int storage_driver_type(int drive);

/* string-extra.h wants it, macOS has none */
void *mempcpy(void *dst, const void *src, size_t n);

#endif /* FSTEST_CONFIG_H */
