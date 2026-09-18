/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DYNBLK_ENGINE_H
#define DYNBLK_ENGINE_H
#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/crc32.h>
typedef u8 db_u8;
typedef u32 db_u32;
typedef u64 db_u64;
#else
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
typedef uint8_t db_u8;
typedef uint32_t db_u32;
typedef uint64_t db_u64;
#endif
#define DE_PAGE 4096U
#define DE_GRAIN 65536U
#define DE_MAX_PARTS 65536U /* Resource guard, not an eagerly allocated array. */
#define DE_MAX_FILES (DE_MAX_PARTS + 1U)
#define DE_NATIVE 1U
#define DE_VMDK 2U
#define DE_AUTO 0U
#define DE_WRITEBACK 0U
#define DE_WRITETHROUGH 1U
#define DE_NONE 2U
#define DE_DIRECTSYNC 3U
#define DE_UNSAFE 4U
#define DE_LIMIT ((db_u64)DE_MAX_PARTS * DE_SPLIT)
#define DE_PART_LIMIT (4000ULL << 20)
#define DE_SPLIT (2ULL << 30)
#define DE_CACHE_DEFAULT_MB 1U
#define DE_CACHE_MAX_MB 64U
#define DE_RETIRED 256U
#define DE_NAME 128U
#define DE_DESCRIPTOR_MAX (1U << 20) /* QEMU reads at most 1 MiB of descriptors. */
/* Exact I/O callbacks return zero or a negative errno. The caller serializes
 * operations on each engine; there is no global storage cache or scratch. */
struct de_io {
    void *ctx;
    void *(*alloc)(void *, size_t);
    void (*free)(void *, void *);
    int (*open)(void *, unsigned int, const char *, bool);
    int (*io)(void *, unsigned int, void *, size_t, db_u64, bool);
    int (*size)(void *, unsigned int, db_u64 *);
    int (*resize)(void *, unsigned int, db_u64);
    int (*sync)(void *, unsigned int);
    int (*sync_dir)(void *);
    /* Optional: release a known-unreferenced, page-aligned file range. */
    int (*punch)(void *, unsigned int, db_u64, db_u64);
    int (*codec)(void *, const char *, bool, const void *, unsigned int,
                 void *, unsigned int *);
};
struct de_extent {
    char name[DE_NAME];
    unsigned int file, entries, entry_bytes, directories;
    db_u64 start, capacity, length, limit, data_start, bitmap_start, bitmap_bytes;
    db_u64 gd, rgd, table_start, next, hint;
    db_u32 *directory, *backup;
    bool dirty_data, dirty_map, trim;
    unsigned int flags;
};
struct de_cache {
    db_u64 offset, age;
    unsigned int part, kind;
    bool valid, dirty;
    db_u8 *bytes;
};
struct de_retired { unsigned int part, count; db_u64 page; };
struct de_engine {
    struct de_io io;
    unsigned int format, cache_mode, nr_extents, extent_slots, nr_cache, nr_retired;
    size_t descriptor_bytes;
    bool readonly, flushing, failed, initialized, punch_disabled;
    db_u64 capacity, span, part_limit, generation, clock;
    db_u64 mapped_grains, stored_bytes, cache_hits, cache_misses, flushes;
    db_u8 uuid[16];
    char codec[16], primary[DE_NAME];
    struct de_extent *extents;
    struct de_cache *cache;
    db_u8 *cache_bytes, *raw, *encoded, *page, *descriptor;
    struct de_retired retired[DE_RETIRED];
    /* One evictable physical-usage bitmap, not one bitmap per extent. */
    db_u8 *usage_bitmap;
    size_t usage_bytes;
    unsigned int usage_part;
    db_u64 punched_bytes, truncated_bytes;
};
#define DE_RECLAIM_COMPACT 1U
#define DE_RECLAIM_ZEROES 2U
struct de_reclaim {
    unsigned int part, phase, flags, done, method;
    db_u64 cursor, scanned_bytes, moved_bytes, unmapped_bytes;
};
/* One bounded step, serialized with ordinary I/O; callers yield between steps. */
int de_reclaim_step(struct de_engine *, struct de_reclaim *);
int de_init(struct de_engine *, const struct de_io *, unsigned int, bool, unsigned int);
void de_destroy(struct de_engine *);
int de_create(struct de_engine *, const char *, unsigned int, db_u64, db_u64,
              const char *, const db_u8[16]);
int de_open(struct de_engine *, const char *, unsigned int);
int de_read(struct de_engine *, db_u64, void *, unsigned int);
int de_write(struct de_engine *, db_u64, const void *, unsigned int);
int de_discard(struct de_engine *, db_u64, unsigned int);
int de_flush(struct de_engine *);
int de_grow(struct de_engine *, db_u64);
int de_check(struct de_engine *, bool);
db_u64 de_memory(const struct de_engine *);
const char *de_format_name(unsigned int);
const char *de_cache_name(unsigned int);
int de_cache_parse(const char *);
db_u64 de_capacity_limit(unsigned int, db_u64);
#endif
