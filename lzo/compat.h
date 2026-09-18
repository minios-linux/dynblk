/* SPDX-License-Identifier: GPL-2.0-only */
/* Userspace definitions for the scalar Linux v6.12 LZO sources. */
/* LZO constants derived from lzodefs.h/linux/lzo.h; original LZO copyright
 * (C) 1996-2012 Markus F.X.J. Oberhumer <markus@oberhumer.com>. */
#ifndef DYNBLK_LZO_COMPAT_H
#define DYNBLK_LZO_COMPAT_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "This helper requires little-endian x86"
#endif
typedef uint32_t u32;
#define likely(x) (x)
#define unlikely(x) (x)
#define noinline __attribute__((noinline))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define min_t(t, a, b) min((t)(a), (t)(b))
#define ALIGN(x, a) (((x) + (a) - 1) & ~((uintptr_t)(a) - 1))
#define IS_ALIGNED(x, a) (!((x) & ((a) - 1)))
#define BUILD_BUG_ON(x) _Static_assert(!(x), #x)
#define COPY4(dst, src) memcpy((dst), (src), 4)
#define COPY8(dst, src) memcpy((dst), (src), 8)
#define LZO_E_OK 0
#define LZO_E_ERROR (-1)
#define LZO_E_INPUT_OVERRUN (-4)
#define LZO_E_OUTPUT_OVERRUN (-5)
#define LZO_E_LOOKBEHIND_OVERRUN (-6)
#define LZO_E_INPUT_NOT_CONSUMED (-8)
#define LZO1X_1_MEM_COMPRESS (8192 * sizeof(unsigned short))
#define LZO_VERSION 1
#define M2_MAX_OFFSET 0x0800
#define M3_MAX_OFFSET 0x4000
#define M4_MAX_OFFSET_V0 0xbfff
#define M4_MAX_OFFSET_V1 0xbffe
#define M2_MAX_LEN 8
#define M3_MAX_LEN 33
#define M4_MAX_LEN 9
#define M3_MARKER 32
#define M4_MARKER 16
#define MIN_ZERO_RUN_LENGTH 4
#define MAX_ZERO_RUN_LENGTH (2047 + MIN_ZERO_RUN_LENGTH)
#define lzo_dict_t unsigned short
#define D_BITS 13
#define D_SIZE (1u << D_BITS)
#define D_MASK (D_SIZE - 1)

static inline uint16_t get_unaligned_le16(const void *p)
{
	uint16_t v;
	memcpy(&v, p, sizeof(v));
	return v;
}
static inline uint32_t get_unaligned_le32(const void *p)
{
	uint32_t v;
	memcpy(&v, p, sizeof(v));
	return v;
}
static inline void put_unaligned_le32(uint32_t v, void *p)
{
	memcpy(p, &v, sizeof(v));
}
int dynblk_lzo1x_decompress_safe(const unsigned char *, size_t, unsigned char *, size_t *);
int lzo1x_1_compress(const unsigned char *, size_t, unsigned char *, size_t *, void *);
int lzorle1x_1_compress(const unsigned char *, size_t, unsigned char *, size_t *, void *);
#endif
