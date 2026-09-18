/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DYNBLK_COMMON_H
#define DYNBLK_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int dynblk_safe_path(const char *path, bool allow_missing_final,
			 char *error, size_t error_size);
int dynblk_parse_size(const char *text, uint64_t *value,
		      char *error, size_t error_size);
int dynblk_aligned_size(uint64_t value, uint64_t minimum, uint64_t maximum,
			char *error, size_t error_size);
int dynblk_codec_field(const uint8_t field[16], char output[16],
		       char *error, size_t error_size);
bool dynblk_codec_name(const char *name);

#endif
