/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DYNBLK_UAPI_H
#define DYNBLK_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define DYNBLK_ABI_VERSION 1U
#define DYNBLK_BLOCK_SIZE 4096U
#define DYNBLK_MAX_PARTS 65536U
#define DYNBLK_TREE_HEIGHT 2U
#define DYNBLK_COMPRESSION_REGION 65536ULL
#define DYNBLK_MAX_CAPACITY (DYNBLK_MAX_PARTS * (2ULL << 30))
#define DYNBLK_MAX_DEVICES 256U
#define DYNBLK_PATH_MAX 4096U
#define DYNBLK_CODEC_MAX 16U
#define DYNBLK_ATTACH_CREATE 1U
#define DYNBLK_ATTACH_READ_ONLY 2U
#define DYNBLK_STATUS_FENCED 1U
#define DYNBLK_STATUS_READ_ONLY 2U
#define DYNBLK_STATUS_AUTOCLEAR 4U
#define DYNBLK_DEFAULT_MAP_MEMORY_MB 0U
#define DYNBLK_MAX_MAP_MEMORY_MB 64U

#define DYNBLK_FORMAT_AUTO 0U
#define DYNBLK_FORMAT_NATIVE 1U
#define DYNBLK_FORMAT_VMDK 2U
#define DYNBLK_CAP_RECLAIM 1U
#define DYNBLK_CAP_GROW 2U
#define DYNBLK_CAP_COMPRESSION 4U

struct dynblk_status {
	__u32 version, flags;
	__u8 uuid[16], algorithm[16];
	__u64 capacity, original_bytes, stored_bytes, physical_bytes;
	__u64 generation, part_limit;
	__u32 active_parts, block_size, max_parts, tree_height;
	__u64 map_memory_bytes, max_capacity, compression_region_bytes;
    __u32 storage_format, cache_mode;
    __u64 engine_memory_bytes, cache_hits, cache_misses, flush_count;
    __u32 capabilities, reserved;
};

struct dynblk_attach {
	__u32 version, flags;
	__s32 device;
	__u32 map_memory_mb;
	__u64 size_bytes, part_limit_bytes, cookie;
	__u8 algorithm[DYNBLK_CODEC_MAX];
	__u8 path[DYNBLK_PATH_MAX];
    __u32 storage_format, cache_mode;
};

struct dynblk_detach {
	__u32 version, device;
	__u64 cookie;
};

/* A bounded online step. Keep the whole-disk fd open across all steps.
 * Counters are per-step; user space accumulates them. No pointers in compat ABI. */
#define DYNBLK_RECLAIM_COMPACT 1U
#define DYNBLK_RECLAIM_ZEROES 2U
struct dynblk_reclaim {
    __u32 version, flags;
    __u64 cookie;
    __u32 part, phase;
    __u64 cursor;
    __u64 scanned_bytes, moved_bytes, unmapped_bytes;
    __u64 punch_bytes, truncated_bytes;
    __u32 done, method;
};

#define DYNBLK_GET _IOR(0xd7, 0, struct dynblk_status)
#define DYNBLK_GROW _IOW(0xd7, 1, __u64)
#define DYNBLK_ATTACH _IOWR(0xd7, 2, struct dynblk_attach)
#define DYNBLK_DETACH _IOW(0xd7, 3, struct dynblk_detach)
#define DYNBLK_COOKIE _IOR(0xd7, 4, __u64)
/* Arm last-close teardown. The caller must hold a whole-disk fd and its cookie. */
#define DYNBLK_AUTOCLEAR _IOW(0xd7, 5, __u64)
#define DYNBLK_RECLAIM _IOWR(0xd7, 6, struct dynblk_reclaim)

#endif
