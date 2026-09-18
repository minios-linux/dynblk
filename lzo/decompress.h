/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DYNBLK_LZO_DECOMPRESS_H
#define DYNBLK_LZO_DECOMPRESS_H
#include <stddef.h>
int dynblk_lzo1x_decompress_safe(const unsigned char *input, size_t input_length,
				 unsigned char *output, size_t *output_length);
#endif
