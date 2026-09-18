// SPDX-License-Identifier: GPL-2.0-only
/*
 *  LZO1X Compressor from LZO
 *  Copyright (C) 1996-2012 Markus F.X.J. Oberhumer <markus@oberhumer.com>
 *  http://www.oberhumer.com/opensource/lzo/
 *  Changed for Linux kernel use by Nitin Gupta <nitingupta910@gmail.com>
 *  and Richard Purdie <rpurdie@openedhand.com>.
 *
 *  TEST ONLY: Linux v6.12 lib/lzo/lzo1x_compress.c scalar path. Kernel includes
 *  and exports replaced with compat.h; architecture-specific fast paths omitted.
 *  Aligned scalar reads use memcpy to avoid userspace strict-aliasing violations.
 */
#include "compat.h"

static noinline size_t
lzo1x_1_do_compress(const unsigned char *in, size_t in_len,
		    unsigned char *out, size_t *out_len,
		    size_t ti, void *wrkmem, signed char *state_offset,
		    const unsigned char bitstream_version)
{
	const unsigned char *ip;
	unsigned char *op;
	const unsigned char * const in_end = in + in_len;
	const unsigned char * const ip_end = in + in_len - 20;
	const unsigned char *ii;
	lzo_dict_t * const dict = (lzo_dict_t *) wrkmem;
	op = out;
	ip = in;
	ii = ip;
	ip += ti < 4 ? 4 - ti : 0;
	for (;;) {
		const unsigned char *m_pos = NULL;
		size_t t, m_len, m_off;
		u32 dv;
		u32 run_length = 0;
literal:
		ip += 1 + ((ip - ii) >> 5);
next:
		if (unlikely(ip >= ip_end))
			break;
		dv = get_unaligned_le32(ip);
		if (dv == 0 && bitstream_version) {
			const unsigned char *ir = ip + 4;
			const unsigned char *limit = min(ip_end, ip + MAX_ZERO_RUN_LENGTH + 1);
			while ((ir < (const unsigned char *) ALIGN((uintptr_t)ir, 4)) &&
			       (ir < limit) && (*ir == 0))
				ir++;
			if (IS_ALIGNED((uintptr_t)ir, 4)) {
				for (; (ir + 4) <= limit; ir += 4) {
					dv = get_unaligned_le32(ir);
					if (dv) {
						ir += __builtin_ctz(dv) >> 3;
						break;
					}
				}
			}
			while (likely(ir < limit) && unlikely(*ir == 0))
				ir++;
			run_length = ir - ip;
			if (run_length > MAX_ZERO_RUN_LENGTH)
				run_length = MAX_ZERO_RUN_LENGTH;
		} else {
			t = ((dv * 0x1824429d) >> (32 - D_BITS)) & D_MASK;
			m_pos = in + dict[t];
			dict[t] = (lzo_dict_t) (ip - in);
			if (unlikely(dv != get_unaligned_le32(m_pos)))
				goto literal;
		}
		ii -= ti;
		ti = 0;
		t = ip - ii;
		if (t != 0) {
			if (t <= 3) {
				op[*state_offset] |= t;
				COPY4(op, ii);
				op += t;
			} else if (t <= 16) {
				*op++ = (t - 3);
				COPY8(op, ii);
				COPY8(op + 8, ii + 8);
				op += t;
			} else {
				if (t <= 18) {
					*op++ = (t - 3);
				} else {
					size_t tt = t - 18;
					*op++ = 0;
					while (unlikely(tt > 255)) {
						tt -= 255;
						*op++ = 0;
					}
					*op++ = tt;
				}
				do {
					COPY8(op, ii);
					COPY8(op + 8, ii + 8);
					op += 16;
					ii += 16;
					t -= 16;
				} while (t >= 16);
				if (t > 0) do {
					*op++ = *ii++;
				} while (--t > 0);
			}
		}
		if (unlikely(run_length)) {
			ip += run_length;
			run_length -= MIN_ZERO_RUN_LENGTH;
			put_unaligned_le32((run_length << 21) | 0xfffc18 | (run_length & 0x7), op);
			op += 4;
			run_length = 0;
			*state_offset = -3;
			goto finished_writing_instruction;
		}
		m_len = 4;
		if (unlikely(ip[m_len] == m_pos[m_len])) {
			do {
				m_len += 1;
				if (ip[m_len] != m_pos[m_len]) break;
				m_len += 1;
				if (ip[m_len] != m_pos[m_len]) break;
				m_len += 1;
				if (ip[m_len] != m_pos[m_len]) break;
				m_len += 1;
				if (ip[m_len] != m_pos[m_len]) break;
				m_len += 1;
				if (ip[m_len] != m_pos[m_len]) break;
				m_len += 1;
				if (ip[m_len] != m_pos[m_len]) break;
				m_len += 1;
				if (ip[m_len] != m_pos[m_len]) break;
				m_len += 1;
				if (unlikely(ip + m_len >= ip_end)) goto m_len_done;
			} while (ip[m_len] == m_pos[m_len]);
		}
m_len_done:
		m_off = ip - m_pos;
		ip += m_len;
		if (m_len <= M2_MAX_LEN && m_off <= M2_MAX_OFFSET) {
			m_off -= 1;
			*op++ = (((m_len - 1) << 5) | ((m_off & 7) << 2));
			*op++ = (m_off >> 3);
		} else if (m_off <= M3_MAX_OFFSET) {
			m_off -= 1;
			if (m_len <= M3_MAX_LEN)
				*op++ = (M3_MARKER | (m_len - 2));
			else {
				m_len -= M3_MAX_LEN;
				*op++ = M3_MARKER | 0;
				while (unlikely(m_len > 255)) {
					m_len -= 255;
					*op++ = 0;
				}
				*op++ = (m_len);
			}
			*op++ = (m_off << 2);
			*op++ = (m_off >> 6);
		} else {
			m_off -= 0x4000;
			if (m_len <= M4_MAX_LEN)
				*op++ = (M4_MARKER | ((m_off >> 11) & 8) | (m_len - 2));
			else {
				if (unlikely(((m_off & 0x403f) == 0x403f) && (m_len >= 261)
					     && (m_len <= 264)) && likely(bitstream_version)) {
					/* Avoid ambiguous RLE/block-copy encodings, as upstream. */
					ip -= m_len - 260;
					m_len = 260;
				}
				m_len -= M4_MAX_LEN;
				*op++ = (M4_MARKER | ((m_off >> 11) & 8));
				while (unlikely(m_len > 255)) {
					m_len -= 255;
					*op++ = 0;
				}
				*op++ = (m_len);
			}
			*op++ = (m_off << 2);
			*op++ = (m_off >> 6);
		}
		*state_offset = -2;
finished_writing_instruction:
		ii = ip;
		goto next;
	}
	*out_len = op - out;
	return in_end - (ii - ti);
}

static int lzogeneric1x_1_compress(const unsigned char *in, size_t in_len,
		     unsigned char *out, size_t *out_len,
		     void *wrkmem, const unsigned char bitstream_version)
{
	const unsigned char *ip = in;
	unsigned char *op = out;
	unsigned char *data_start;
	size_t l = in_len;
	size_t t = 0;
	signed char state_offset = -2;
	unsigned int m4_max_offset;
	/* Upstream version prefix: ordinary LZO is v0, LZO-RLE is v1. */
	if (bitstream_version > 0) {
		*op++ = 17;
		*op++ = bitstream_version;
		m4_max_offset = M4_MAX_OFFSET_V1;
	} else {
		m4_max_offset = M4_MAX_OFFSET_V0;
	}
	data_start = op;
	while (l > 20) {
		size_t ll = min_t(size_t, l, m4_max_offset + 1);
		uintptr_t ll_end = (uintptr_t) ip + ll;
		if ((ll_end + ((t + ll) >> 5)) <= ll_end) break;
		BUILD_BUG_ON(D_SIZE * sizeof(lzo_dict_t) > LZO1X_1_MEM_COMPRESS);
		memset(wrkmem, 0, D_SIZE * sizeof(lzo_dict_t));
		t = lzo1x_1_do_compress(ip, ll, op, out_len, t, wrkmem, &state_offset, bitstream_version);
		ip += ll;
		op += *out_len;
		l -= ll;
	}
	t += l;
	if (t > 0) {
		const unsigned char *ii = in + in_len - t;
		if (op == data_start && t <= 238) {
			*op++ = (17 + t);
		} else if (t <= 3) {
			op[state_offset] |= t;
		} else if (t <= 18) {
			*op++ = (t - 3);
		} else {
			size_t tt = t - 18;
			*op++ = 0;
			while (tt > 255) {
				tt -= 255;
				*op++ = 0;
			}
			*op++ = tt;
		}
		if (t >= 16) do {
			COPY8(op, ii);
			COPY8(op + 8, ii + 8);
			op += 16;
			ii += 16;
			t -= 16;
		} while (t >= 16);
		if (t > 0) do {
			*op++ = *ii++;
		} while (--t > 0);
	}
	*op++ = M4_MARKER | 1;
	*op++ = 0;
	*op++ = 0;
	*out_len = op - out;
	return LZO_E_OK;
}

int lzo1x_1_compress(const unsigned char *in, size_t in_len,
		     unsigned char *out, size_t *out_len, void *wrkmem)
{
	return lzogeneric1x_1_compress(in, in_len, out, out_len, wrkmem, 0);
}
int lzorle1x_1_compress(const unsigned char *in, size_t in_len,
		     unsigned char *out, size_t *out_len, void *wrkmem)
{
	return lzogeneric1x_1_compress(in, in_len, out, out_len, wrkmem, LZO_VERSION);
}
