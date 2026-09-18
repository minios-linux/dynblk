// SPDX-License-Identifier: GPL-2.0-only
/* Actual Linux-derived compressor/decompressor with exact-size ASan buffers. */
#include "compat.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t state = 0x91e10da5;
static uint32_t random32(void)
{
	state ^= state << 13;
	state ^= state >> 17;
	state ^= state << 5;
	return state;
}

int main(void)
{
	unsigned char compressed[70000], work[LZO1X_1_MEM_COMPRESS];
	size_t truncations = 0;
	for (unsigned trial = 0; trial < 1024; trial++) {
		size_t length = trial < 32 ? trial : trial % 3 == 0 ? 4096 :
			trial % 3 == 1 ? 65536 : random32() % 65537;
		unsigned char *input = malloc(length ? length : 1);
		unsigned char *output = malloc(length ? length : 1);
		assert(input && output);
		for (size_t i = 0; i < length; i++) {
			u32 r = random32();
			input[i] = trial % 4 == 0 ? 0 : trial % 4 == 1 ? 'A' :
				trial % 4 == 2 ? (i % 4096 < 4000 ? 0 : r) : r;
		}
		for (unsigned rle = 0; rle < 2; rle++) {
			size_t encoded = sizeof(compressed), decoded = length;
			int ret = rle ? lzorle1x_1_compress(input, length, compressed, &encoded, work) :
				lzo1x_1_compress(input, length, compressed, &encoded, work);
			assert(ret == 0 && encoded <= sizeof(compressed));
			unsigned char *exact = malloc(encoded);
			assert(exact);
			memcpy(exact, compressed, encoded);
			ret = dynblk_lzo1x_decompress_safe(exact, encoded, output, &decoded);
			assert(ret == 0 && decoded == length && !memcmp(input, output, length));
			for (size_t prefix = 0; prefix < encoded && prefix < 64; prefix++) {
				unsigned char *short_input = malloc(prefix ? prefix : 1);
				assert(short_input);
				memcpy(short_input, compressed, prefix);
				decoded = length;
				ret = dynblk_lzo1x_decompress_safe(short_input, prefix, output, &decoded);
				/* A short v1 prefix can itself be a valid empty v0 stream. */
				assert(ret != 0 || decoded != length);
				assert(decoded <= length);
				free(short_input);
				truncations++;
			}
			free(exact);
		}
		free(input);
		free(output);
	}
	for (unsigned trial = 0; trial < 20000; trial++) {
		size_t length = trial % 1000 == 0 ? 65536 : random32() % 257;
		size_t capacity = random32() % 65537, decoded = capacity;
		unsigned char *input = malloc(length ? length : 1);
		unsigned char *output = malloc(capacity ? capacity : 1);
		assert(input && output);
		for (size_t i = 0; i < length; i++) input[i] = random32();
		if (length >= 5 && trial % 2) { input[0] = 17; input[1] = 1; }
		(void)dynblk_lzo1x_decompress_safe(input, length, output, &decoded);
		assert(decoded <= capacity);
		free(input);
		free(output);
	}
	printf("PASS 2048 Linux LZO/LZO-RLE roundtrips, %zu rejected truncations, 20000 malformed-input bounds trials\n", truncations);
	return 0;
}
