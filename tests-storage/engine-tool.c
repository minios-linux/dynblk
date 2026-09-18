/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Test-only image exerciser. Never installed; operates on ordinary files. */
#include "../dynblk_host.h"
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
static void pattern(unsigned char *buf, size_t n, db_u64 off, unsigned int kind)
{
    size_t i;
    for (i = 0; i < n; i++) {
        db_u64 v = (off + i) * 0x9e3779b97f4a7c15ULL;
        v ^= v >> 29; v *= 0xbf58476d1ce4e5b9ULL; v ^= v >> 31;
        buf[i] = kind < 256 ? kind : v;
    }
}
static db_u64 number(const char *s)
{
    char *end;
    db_u64 v;
    errno = 0; v = strtoull(s, &end, 0);
    if (errno || !*s || *end || *s == '-') { fprintf(stderr, "invalid integer\n"); exit(2); }
    return v;
}
int main(int argc, char **argv)
{
    struct de_engine e = {0};
    struct de_host h;
    struct de_io io;
    unsigned char uuid[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    unsigned char *buf = calloc(1, DE_GRAIN), *expected = calloc(1, DE_GRAIN);
    unsigned int mode = DE_WRITEBACK;
    db_u64 pos, bytes, begin_sync;
    unsigned int kind, n, i;
    bool readonly;
    int ret;
    if (argc < 3 || !buf || !expected) return 2;
    readonly = !strcmp(argv[1], "read") || !strcmp(argv[1], "check");
    if (argc >= 7) { ret = de_cache_parse(argv[6]); if (ret < 0) return 2; mode = ret; }
    ret = dh_init(&h, &io, argv[2], readonly, mode == DE_NONE || mode == DE_DIRECTSYNC, false);
    if (ret) goto host_done;
    if (getenv("DYNBLK_TEST_NO_PUNCH")) io.punch = NULL;
    ret = de_init(&e, &io, 1, readonly, mode);
    if (ret) goto done;
    if (!strcmp(argv[1], "create")) {
        if (argc != 7) { ret = -EINVAL; goto done; }
        ret = de_create(&e, strrchr(argv[2], '/') + 1,
            !strcmp(argv[3], "dynblk") ? DE_NATIVE : DE_VMDK, number(argv[4]), 0, argv[5], uuid);
        if (!ret) ret = de_flush(&e);
        goto report;
    }
    ret = de_open(&e, strrchr(argv[2], '/') + 1, DE_AUTO);
    if (ret) goto done;
    begin_sync = h.sync_count;
    if (!strcmp(argv[1], "check")) ret = de_check(&e, true);
    else if (!strcmp(argv[1], "reclaim") || !strcmp(argv[1], "compact") || !strcmp(argv[1], "zeroes")) {
        struct de_reclaim state = {0};
        if (!strcmp(argv[1], "compact")) state.flags |= DE_RECLAIM_COMPACT;
        if (!strcmp(argv[1], "zeroes")) state.flags |= DE_RECLAIM_ZEROES;
        do { ret = de_reclaim_step(&e, &state); } while (!ret && !state.done);
        fprintf(stderr, "reclaim: moved=%llu unmapped=%llu punched=%llu truncated=%llu\n",
            (unsigned long long)state.moved_bytes, (unsigned long long)state.unmapped_bytes,
            (unsigned long long)e.punched_bytes, (unsigned long long)e.truncated_bytes);
    }
    else if (!strcmp(argv[1], "grow")) ret = argc >= 4 ? de_grow(&e, number(argv[3])) : -EINVAL;
    else if (!strcmp(argv[1], "stress")) {
        unsigned char *model = calloc(1, 1048576);
        unsigned int state = 71;
        if (!model || e.capacity < 1048576) { free(model); ret = -ENOMEM; goto done; }
        for (pos = 0; pos < 1048576; pos += DE_GRAIN) {
            ret = de_discard(&e, pos, DE_GRAIN);
            if (ret) break;
        }
        for (i = 0; i < 1200 && !ret; i++) {
            state = state * 1664525U + 1013904223U;
            pos = (state % 2048) * 512; n = i % 3 ? 512 : 4096;
            if (pos + n > 1048576) n = 512;
            pattern(buf, n, pos, i % 7 ? i % 255 + 1 : 256);
            if (i % 11 == 0) { memset(buf, 0, n); ret = de_discard(&e, pos, n); }
            else ret = de_write(&e, pos, buf, n);
            if (ret) break;
            memcpy(model + pos, buf, n);
            ret = de_read(&e, pos, expected, n);
            if (!ret && memcmp(expected, buf, n)) ret = -EUCLEAN;
            if (!ret && i % 37 == 0) ret = de_flush(&e);
        }
        for (pos = 0; pos < 1048576 && !ret; pos += DE_GRAIN) {
            ret = de_read(&e, pos, expected, DE_GRAIN);
            if (!ret && memcmp(expected, model + pos, DE_GRAIN)) ret = -EUCLEAN;
        }
        free(model);
    } else {
        if (argc < 6) { ret = -EINVAL; goto done; }
        pos = number(argv[3]); bytes = number(argv[4]); kind = number(argv[5]);
        if (bytes > 64ULL * 1048576) { ret = -E2BIG; goto done; }
        while (bytes && !ret) {
            n = bytes > DE_GRAIN ? DE_GRAIN : bytes;
            pattern(buf, n, pos, kind);
            if (!strcmp(argv[1], "write")) ret = de_write(&e, pos, buf, n);
            else if (!strcmp(argv[1], "discard")) ret = de_discard(&e, pos, n);
            else if (!strcmp(argv[1], "read")) {
                ret = de_read(&e, pos, expected, n);
                if (!ret && memcmp(buf, expected, n)) ret = -EUCLEAN;
            } else ret = -EINVAL;
            pos += n; bytes -= n;
        }
    }
    fprintf(stderr, "operation_syncs=%llu\n", (unsigned long long)(h.sync_count - begin_sync));
    if (!ret && !readonly) ret = de_flush(&e);
report:
    if (!ret) printf("{\"format\":\"%s\",\"capacity\":%llu,\"parts\":%u,\"memory\":%llu,"
        "\"mapped_grains\":%llu,\"stored\":%llu,\"flushes\":%llu,\"syncs\":%llu,\"cache_misses\":%llu}\n",
        de_format_name(e.format), (unsigned long long)e.capacity, e.nr_extents,
        (unsigned long long)de_memory(&e), (unsigned long long)e.mapped_grains,
        (unsigned long long)e.stored_bytes, (unsigned long long)e.flushes,
        (unsigned long long)h.sync_count, (unsigned long long)e.cache_misses);
done:
    de_destroy(&e);
host_done:
    if (ret) fprintf(stderr, "engine: %d (%s) %s\n", ret, strerror(-ret), h.error);
    dh_close(&h); free(buf); free(expected);
    return ret ? 1 : 0;
}
