// SPDX-License-Identifier: GPL-2.0-or-later
#define _FILE_OFFSET_BITS 64
#include "dynblk_common.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int set_error(char *error, size_t size, const char *format, ...)
{
	va_list args;
	if (error && size) {
		va_start(args, format);
		vsnprintf(error, size, format, args);
		va_end(args);
	}
	return -1;
}

bool dynblk_codec_name(const char *name)
{
	static const char *const names[] = {
		"none", "lz4", "lz4hc", "lzo", "lzo-rle", "zstd", "deflate", "842"
	};
	size_t i;
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (!strcmp(name, names[i]))
			return true;
	return false;
}
int dynblk_codec_field(const uint8_t field[16], char output[16],
		       char *error, size_t error_size)
{
	size_t nul = 0, i;
	while (nul < 16 && field[nul])
		nul++;
	if (!nul || nul == 16)
		return set_error(error, error_size, "invalid codec string");
	for (i = nul + 1; i < 16; i++)
		if (field[i])
			return set_error(error, error_size, "invalid codec string padding");
	memcpy(output, field, nul);
	output[nul] = 0;
	if (!dynblk_codec_name(output))
		return set_error(error, error_size, "unsupported kernel codec name");
	return 0;
}

int dynblk_aligned_size(uint64_t value, uint64_t minimum, uint64_t maximum,
			char *error, size_t error_size)
{
	if (value < minimum || value > maximum || value % 4096)
		return set_error(error, error_size,
			"size must be 4096-aligned, between %llu and %llu bytes",
			(unsigned long long)minimum, (unsigned long long)maximum);
	return 0;
}
int dynblk_parse_size(const char *text, uint64_t *value,
		      char *error, size_t error_size)
{
	char *end;
	unsigned long long number;
	uint64_t multiplier = 1;
	if (!text || !*text)
		return set_error(error, error_size, "use integer bytes or KiB/MiB/GiB/TiB");
	errno = 0;
	number = strtoull(text, &end, 10);
	if (errno || end == text)
		return set_error(error, error_size, "use integer bytes or KiB/MiB/GiB/TiB");
	if (!strcmp(end, "KiB"))
		multiplier = 1ULL << 10;
	else if (!strcmp(end, "MiB"))
		multiplier = 1ULL << 20;
	else if (!strcmp(end, "GiB"))
		multiplier = 1ULL << 30;
	else if (!strcmp(end, "TiB"))
		multiplier = 1ULL << 40;
	else if (*end)
		return set_error(error, error_size, "use integer bytes or KiB/MiB/GiB/TiB");
	if (number > UINT64_MAX / multiplier)
		return set_error(error, error_size, "size is too large");
	*value = (uint64_t)number * multiplier;
	return 0;
}
static int check_entry(const char *path, bool final, bool allow_missing,
		       char *error, size_t error_size)
{
	struct stat st;
	if (lstat(path, &st)) {
		if (final && allow_missing && errno == ENOENT)
			return 0;
		return set_error(error, error_size, "%s: %s", path, strerror(errno));
	}
	if (S_ISLNK(st.st_mode))
		return set_error(error, error_size, "symlink path refused: %s", path);
	/* Ownership and mode bits are not a path-safety boundary here. FAT/exFAT
	 * synthesize them from mount options, and removable media is commonly
	 * mounted for the desktop user. Symlink refusal plus the kernel's pinned
	 * directory, O_NOFOLLOW/O_EXCL opens and single-link checks protect object
	 * identity; write access to the media only grants the expected ability to
	 * damage or remove the user's own backing image. */
	if (!final && !S_ISDIR(st.st_mode))
		return set_error(error, error_size, "ancestor is not a directory: %s", path);
	if (final && (!S_ISREG(st.st_mode) || st.st_nlink != 1))
		return set_error(error, error_size,
			"expected a single-link regular file: %s", path);
	return 0;
}

static bool dot_component(const char *start, size_t length)
{
	return (length == 1 && start[0] == '.') ||
	       (length == 2 && start[0] == '.' && start[1] == '.');
}
int dynblk_safe_path(const char *path, bool allow_missing_final,
			 char *error, size_t error_size)
{
	char copy[PATH_MAX];
	size_t length, i, component = 1;
	if (!path || path[0] != '/')
		return set_error(error, error_size, "use a canonical absolute path without dot components");
	length = strlen(path);
	if (length < 2 || length >= sizeof(copy) || path[length - 1] == '/')
		return set_error(error, error_size, "use a canonical absolute path without dot components");
	memcpy(copy, path, length + 1);
	if (check_entry("/", false, false, error, error_size))
		return -1;
	for (i = 1; i <= length; i++) {
		if (copy[i] != '/' && copy[i] != 0)
			continue;
		if (i == component || dot_component(copy + component, i - component))
			return set_error(error, error_size,
				"use a canonical absolute path without dot components");
		if (copy[i] == '/') {
			copy[i] = 0;
			if (check_entry(copy, false, false, error, error_size))
				return -1;
			copy[i] = '/';
			component = i + 1;
		} else if (check_entry(copy, true, allow_missing_final, error, error_size)) {
			return -1;
		}
	}
	return 0;
}
