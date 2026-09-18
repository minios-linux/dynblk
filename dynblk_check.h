/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DYNBLK_CHECK_H
#define DYNBLK_CHECK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "dynblk_engine.h"
#define DB_BLOCK DE_PAGE
struct dynblk_inspection {
    char path[4096], compression[16];
    uint8_t uuid[16];
    uint64_t capacity, part_limit, physical_bytes, primary_bytes, generation;
    uint64_t mapped_grains, stored_bytes, memory_bytes;
    uint32_t active_parts, storage_format;
    bool fully_validated;
};
int dynblk_inspect_volume(const char *, bool, struct dynblk_inspection *, char *, size_t);
void dynblk_uuid_string(const uint8_t[16], char[37]);
#endif
