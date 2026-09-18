// SPDX-License-Identifier: GPL-2.0-or-later
#include "dynblk_check.h"
#include "dynblk_host.h"
#include <stdio.h>
#include <string.h>
void dynblk_uuid_string(const uint8_t uuid[16], char out[37])
{
	snprintf(out, 37,
		 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		 uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7],
		 uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
}

int dynblk_inspect_volume(const char *path, bool full, struct dynblk_inspection *result,
                         char *error, size_t error_size)
{
    struct de_engine engine = {0};
    struct de_host host;
    struct de_io io;
    unsigned int i;
    int ret = dh_init(&host, &io, path, true, false, true);
    if (ret) goto host_done;
    ret = de_init(&engine, &io, 1, true, DE_WRITEBACK);
    if (!ret) ret = de_open(&engine, strrchr(path, '/') + 1, DE_AUTO);
    if (!ret && full) ret = de_check(&engine, true);
    if (!ret) {
        memset(result, 0, sizeof(*result));
        snprintf(result->path, sizeof(result->path), "%s", path);
        memcpy(result->uuid, engine.uuid, 16);
        strcpy(result->compression, engine.codec);
        result->storage_format = engine.format; result->capacity = engine.capacity;
        result->part_limit = engine.part_limit; result->generation = engine.generation;
        result->mapped_grains = engine.mapped_grains; result->stored_bytes = engine.stored_bytes;
        result->memory_bytes = de_memory(&engine); result->active_parts = engine.nr_extents;
        result->fully_validated = full;
        for (i = 0; i < engine.nr_extents; i++) result->physical_bytes += engine.extents[i].length;
        if (engine.format == DE_NATIVE) result->primary_bytes = engine.extents[0].length;
        else {
            ret = io.size(io.ctx, 0, &result->primary_bytes);
            result->physical_bytes += result->primary_bytes;
        }
    }
    de_destroy(&engine);
host_done:
    if (ret && error && error_size) {
        if (host.error[0]) snprintf(error, error_size, "%s", host.error);
        else if (ret == -EPROTONOSUPPORT)
            snprintf(error, error_size, "obsolete DBPART01 experimental layout; export with the old driver before conversion");
        else snprintf(error, error_size, "volume validation: %s (%d)", strerror(-ret), ret);
    }
    dh_close(&host);
    return ret ? -1 : 0;
}
