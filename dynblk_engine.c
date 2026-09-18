// SPDX-License-Identifier: GPL-2.0-or-later
/* Shared kernel/userspace storage implementation; see FORMAT.md. */
#include "dynblk_engine.h"
#define DE_MAP 1U
#define DE_BITMAP 2U
#define DE_HEADER_CRC 4092U
static db_u32 de_get32(const void *p)
{
    const db_u8 *b = p;
    return (db_u32)b[0] | (db_u32)b[1] << 8 | (db_u32)b[2] << 16 | (db_u32)b[3] << 24;
}
static db_u64 de_get64(const void *p)
{
    const db_u8 *b = p;
    return de_get32(b) | (db_u64)de_get32(b + 4) << 32;
}
static void de_put32(void *p, db_u32 v)
{
    db_u8 *b = p;
    b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24;
}
static void de_put64(void *p, db_u64 v)
{
    de_put32(p, (db_u32)v); de_put32((db_u8 *)p + 4, v >> 32);
}
static db_u64 de_round(db_u64 n, db_u64 a) { return (n + a - 1) / a * a; }
static bool de_zero(const void *p, size_t n)
{
    const db_u8 *b = p;
    while (n--) if (*b++) return false;
    return true;
}
static db_u32 de_crc(const void *p, size_t n)
{
#ifdef __KERNEL__
    return crc32_le(~0U, p, n) ^ ~0U;
#else
    const db_u8 *b = p;
    db_u32 crc = ~0U;
    while (n--) {
        unsigned int i;
        crc ^= *b++;
        for (i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return crc ^ ~0U;
#endif
}
static int de_error(struct de_engine *e, int ret)
{
    if (ret && ret != -EROFS && ret != -EINVAL && ret != -EOPNOTSUPP)
        e->failed = true;
    return ret;
}
const char *de_format_name(unsigned int f)
{
    return f == DE_NATIVE ? "dynblk" : f == DE_VMDK ? "vmdk" : "auto";
}
const char *de_cache_name(unsigned int c)
{
    static const char *const names[] = {"writeback", "writethrough", "none", "directsync", "unsafe"};
    return c < 5 ? names[c] : "invalid";
}
int de_cache_parse(const char *s)
{
    unsigned int i;
    for (i = 0; i < 5; i++) if (!strcmp(s, de_cache_name(i))) return (int)i;
    return -EINVAL;
}
static bool de_name_ok(const char *s)
{
    size_t n = strlen(s), i;
    if (!n || n >= DE_NAME || s[0] == '.' || s[0] == '-') return false;
    for (i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
    }
    return true;
}
static int de_sync(struct de_engine *e, unsigned int file)
{
    if (e->readonly || e->cache_mode == DE_UNSAFE) return 0;
    return e->io.sync(e->io.ctx, file);
}
static int de_io(struct de_engine *e, unsigned int f, void *p, size_t n, db_u64 off, bool wr)
{
    if (wr && e->readonly) return -EROFS;
    return e->io.io(e->io.ctx, f, p, n, off, wr);
}
/* Extent vectors grow with actual geometry; large support costs small disks no RAM. */
static int de_reserve_extents(struct de_engine *e, unsigned int need)
{
    struct de_extent *p;
    unsigned int slots = e->extent_slots ? e->extent_slots : 8;
    if (need > DE_MAX_PARTS) return -EFBIG;
    if (need <= e->extent_slots) return 0;
    while (slots < need) slots *= 2;
    p = e->io.alloc(e->io.ctx, (size_t)slots * sizeof(*p));
    if (!p) return -ENOMEM;
    memset(p, 0, (size_t)slots * sizeof(*p));
    if (e->extent_slots) memcpy(p, e->extents, (size_t)e->extent_slots * sizeof(*p));
    e->io.free(e->io.ctx, e->extents);
    e->extents = p; e->extent_slots = slots;
    return 0;
}
static int de_reserve_descriptor(struct de_engine *e, size_t need)
{
    db_u8 *p;
    size_t size = e->descriptor_bytes ? e->descriptor_bytes : DE_PAGE;
    if (need > DE_DESCRIPTOR_MAX) return -E2BIG;
    if (need <= e->descriptor_bytes) return 0;
    while (size < need) size *= 2;
    p = e->io.alloc(e->io.ctx, size);
    if (!p) return -ENOMEM;
    memset(p, 0, size);
    if (e->descriptor_bytes) memcpy(p, e->descriptor, e->descriptor_bytes);
    e->io.free(e->io.ctx, e->descriptor);
    e->io.free(e->io.ctx, e->usage_bitmap);
    e->descriptor = p; e->descriptor_bytes = size;
    return 0;
}
static db_u64 de_native_span(db_u64 limit)
{
    db_u64 span = DE_GRAIN;
    while (span * 2 <= limit / 2 && span < (1ULL << 30)) span *= 2;
    return span;
}
db_u64 de_capacity_limit(unsigned int format, db_u64 part_limit)
{
    return (db_u64)DE_MAX_PARTS * (format == DE_NATIVE ?
        de_native_span(part_limit ? part_limit : DE_PART_LIMIT) : DE_SPLIT);
}
int de_init(struct de_engine *e, const struct de_io *io, unsigned int cache_mb,
            bool readonly, unsigned int mode)
{
    unsigned int i;
    memset(e, 0, sizeof(*e));
    e->io = *io; e->readonly = readonly; e->cache_mode = mode;
    e->usage_part = ~0U;
    if (mode > DE_UNSAFE || cache_mb > DE_CACHE_MAX_MB) return -EINVAL;
    if (!cache_mb) cache_mb = DE_CACHE_DEFAULT_MB;
    e->nr_cache = cache_mb * (1048576U / DE_PAGE);
    e->cache = io->alloc(io->ctx, sizeof(*e->cache) * e->nr_cache);
    e->cache_bytes = io->alloc(io->ctx, (size_t)e->nr_cache * DE_PAGE);
    e->raw = io->alloc(io->ctx, DE_GRAIN);
    e->encoded = io->alloc(io->ctx, DE_GRAIN * 2);
    e->page = io->alloc(io->ctx, DE_PAGE);
    if (de_reserve_descriptor(e, DE_PAGE)) return -ENOMEM;
    if (!e->cache || !e->cache_bytes || !e->raw || !e->encoded || !e->page || !e->descriptor)
        return -ENOMEM;
    memset(e->cache, 0, sizeof(*e->cache) * e->nr_cache);
    for (i = 0; i < e->nr_cache; i++) e->cache[i].bytes = e->cache_bytes + (size_t)i * DE_PAGE;
    return 0;
}
void de_destroy(struct de_engine *e)
{
    unsigned int i;
    if (!e->io.free) return;
    if (e->extents) for (i = 0; i < e->extent_slots; i++) {
        e->io.free(e->io.ctx, e->extents[i].directory);
        e->io.free(e->io.ctx, e->extents[i].backup);
    }
    e->io.free(e->io.ctx, e->extents); e->io.free(e->io.ctx, e->cache);
    e->io.free(e->io.ctx, e->cache_bytes); e->io.free(e->io.ctx, e->raw);
    e->io.free(e->io.ctx, e->encoded); e->io.free(e->io.ctx, e->page);
    e->io.free(e->io.ctx, e->descriptor);
    e->io.free(e->io.ctx, e->usage_bitmap);
    memset(e, 0, sizeof(*e));
}
db_u64 de_memory(const struct de_engine *e)
{
    db_u64 n = sizeof(*e) + sizeof(*e->extents) * e->extent_slots +
        (db_u64)e->nr_cache * (DE_PAGE + sizeof(*e->cache)) + DE_GRAIN * 3 + DE_PAGE + e->descriptor_bytes + e->usage_bytes;
    unsigned int i;
    for (i = 0; i < e->nr_extents; i++) n += e->extents[i].directories * 4ULL * (e->extents[i].backup ? 2 : 1);
    return n;
}
static int de_cache_get(struct de_engine *e, unsigned int part, db_u64 off,
                         unsigned int kind, struct de_cache **out)
{
    unsigned int i, victim = 0;
    db_u64 age = ~(db_u64)0;
    struct de_cache *c;
    int ret;
    off = off / DE_PAGE * DE_PAGE;
    for (i = 0; i < e->nr_cache; i++) {
        c = &e->cache[i];
        if (c->valid && c->part == part && c->offset == off) {
            if (c->kind != kind) return -EUCLEAN;
            c->age = ++e->clock; e->cache_hits++; *out = c; return 0;
        }
        if (!c->valid) { victim = i; age = 0; }
        else if (age && c->age < age) { age = c->age; victim = i; }
    }
    c = &e->cache[victim];
    if (c->valid && c->dirty) {
        if (!e->flushing) {
            ret = de_flush(e);
            if (ret) return ret;
            return de_cache_get(e, part, off, kind, out);
        }
        if (c->kind != DE_BITMAP) return -EUCLEAN;
        ret = de_io(e, e->extents[c->part].file, c->bytes, DE_PAGE, c->offset, true);
        if (ret) return ret;
    }
    c->valid = false; c->dirty = false;
    ret = de_io(e, e->extents[part].file, c->bytes, DE_PAGE, off, false);
    if (ret) return ret;
    c->valid = true; c->part = part; c->offset = off; c->kind = kind;
    c->age = ++e->clock; e->cache_misses++; *out = c;
    return 0;
}
static int de_bitmap(struct de_engine *e, unsigned int part, db_u64 page, int change, bool *used)
{
    struct de_extent *x = &e->extents[part];
    struct de_cache *c;
    db_u64 off = x->bitmap_start + page / 8;
    unsigned int b = page % 8, index = off % DE_PAGE;
    int ret;
    if (page >= x->limit / DE_PAGE) return -EUCLEAN;
    ret = de_cache_get(e, part, off, DE_BITMAP, &c);
    if (ret) return ret;
    if (used) *used = !!(c->bytes[index] & (1U << b));
    if (change >= 0) {
        if (change) c->bytes[index] |= 1U << b;
        else c->bytes[index] &= ~(1U << b);
        c->dirty = true;
    }
    return 0;
}
static bool de_used(const db_u8 *bitmap, db_u64 page)
{
    return !!(bitmap[page / 8] & (1U << (page % 8)));
}
static void de_mark(db_u8 *bitmap, db_u64 page, unsigned int count, bool used)
{
    while (count--) {
        if (used) bitmap[page / 8] |= 1U << (page % 8);
        else bitmap[page / 8] &= ~(1U << (page % 8));
        page++;
    }
}
static void de_usage_mark(struct de_engine *e, unsigned int part,
                           db_u64 page, unsigned int count, bool used)
{
    if (e->usage_bitmap && e->usage_part == part)
        de_mark(e->usage_bitmap, page, count, used);
}
static int de_usage(struct de_engine *e, unsigned int part);
static int de_trim_usage(struct de_engine *e, unsigned int part);
static int de_punch(struct de_engine *e, unsigned int part, db_u64 off, db_u64 bytes)
{
    int ret;
    if (!bytes || !e->io.punch || e->punch_disabled || e->cache_mode == DE_UNSAFE) return 0;
    ret = e->io.punch(e->io.ctx, e->extents[part].file, off, bytes);
    if (ret == -EOPNOTSUPP || ret == -ENOSYS) {
        e->punch_disabled = true;
        return 0; /* No data loss: this is already free storage. */
    }
    if (!ret) e->punched_bytes += bytes; /* Submitted ranges, NOT allocated-byte savings. */
    return ret;
}
static int de_map(struct de_engine *e, unsigned int part, db_u64 grain,
                   db_u8 value[16], bool write)
{
    struct de_extent *x = &e->extents[part];
    db_u64 table = grain / x->entries, off;
    unsigned int index = grain % x->entries;
    struct de_cache *c;
    int ret;
    if (table >= x->directories) return -EUCLEAN;
    off = (db_u64)x->directory[table] * 512 + index * x->entry_bytes;
    ret = de_cache_get(e, part, off, DE_MAP, &c);
    if (ret) return ret;
    if (!write) {
        memset(value, 0, 16);
        memcpy(value, c->bytes + off % DE_PAGE, x->entry_bytes);
        return 0;
    }
    memcpy(c->bytes + off % DE_PAGE, value, x->entry_bytes); c->dirty = true;
    /* Keep the one-part usage cache coherent. Old storage is still reserved
     * until BOTH metadata copies have passed the flush barrier. */
    {
        db_u64 loc = e->format == DE_NATIVE ? de_get64(value) : (db_u64)de_get32(value) * 512;
        unsigned int len = e->format == DE_NATIVE ? de_get32(value + 8) : DE_GRAIN;
        if (loc && !(e->format == DE_VMDK && loc == 512))
            de_usage_mark(e, part, loc / DE_PAGE, de_round(len, DE_PAGE) / DE_PAGE, true);
    }
    if (x->backup) {
        off = (db_u64)x->backup[table] * 512 + index * x->entry_bytes;
        ret = de_cache_get(e, part, off, DE_MAP, &c);
        if (ret) return ret;
        memcpy(c->bytes + off % DE_PAGE, value, x->entry_bytes); c->dirty = true;
    }
    return 0;
}
/* Newly referenced payloads become durable BEFORE their mappings. Retired
 * runs remain allocated until the mapping barrier. Dirty-cache eviction uses
 * this same protocol; unsafe deliberately omits durability barriers. */
int de_flush(struct de_engine *e)
{
    unsigned int i, j;
    int ret = 0;
    if (e->readonly) return 0;
    if (e->failed) return -EIO;
    if (e->flushing) return -EDEADLK;
    e->flushing = true;
    for (i = 0; i < e->nr_extents; i++) if (e->extents[i].dirty_data) {
        ret = de_sync(e, e->extents[i].file);
        if (ret) goto done;
        e->extents[i].dirty_data = false;
    }
    for (i = 0; i < e->nr_cache; i++) {
        struct de_cache *c = &e->cache[i];
        if (!c->valid || !c->dirty || c->kind != DE_MAP) continue;
        ret = de_io(e, e->extents[c->part].file, c->bytes, DE_PAGE, c->offset, true);
        if (ret) goto done;
        e->extents[c->part].dirty_map = true;
    }
    for (i = 0; i < e->nr_extents; i++) if (e->extents[i].dirty_map) {
        ret = de_sync(e, e->extents[i].file);
        if (ret) goto done;
        e->extents[i].dirty_map = false;
    }
    for (i = 0; i < e->nr_cache; i++)
        if (e->cache[i].kind == DE_MAP) e->cache[i].dirty = false;
    for (i = 0; i < e->nr_retired; i++) {
        struct de_retired *r = &e->retired[i];
        if (e->format == DE_NATIVE) for (j = 0; j < r->count; j++) {
            ret = de_bitmap(e, r->part, r->page + j, 0, NULL);
            if (ret) goto done;
        }
        de_usage_mark(e, r->part, r->page, r->count, false);
        if (e->extents[r->part].hint > r->page) e->extents[r->part].hint = r->page;
        /* Mappings are durable before punching; never change live payloads. */
        ret = de_punch(e, r->part, r->page * DE_PAGE, (db_u64)r->count * DE_PAGE);
        if (ret) goto done;
    }
    e->nr_retired = 0;
    for (i = 0; i < e->nr_cache; i++) {
        struct de_cache *c = &e->cache[i];
        if (!c->valid || !c->dirty) continue;
        ret = de_io(e, e->extents[c->part].file, c->bytes, DE_PAGE, c->offset, true);
        if (ret) goto done;
        c->dirty = false;
    }
    /* The native allocator is reconstructed from mappings at writable open;
     * allocator durability is not a prerequisite for safely reading data. */
    for (i = 0; i < e->nr_extents; i++) {
        struct de_extent *x = &e->extents[i];
        db_u64 last;
        bool used;
        if (!x->trim || e->cache_mode == DE_UNSAFE) continue;
        if (e->format == DE_VMDK) {
            ret = de_trim_usage(e, i);
            if (ret) goto done;
            continue;
        }
        x->trim = false; last = de_round(x->length, DE_PAGE) / DE_PAGE;
        while (last > x->data_start / DE_PAGE) {
            ret = de_bitmap(e, i, last - 1, -1, &used);
            if (ret) goto done;
            if (used) break;
            last--;
        }
        if (last * DE_PAGE < x->length) {
            ret = e->io.resize(e->io.ctx, x->file, last * DE_PAGE);
            if (!ret) ret = de_sync(e, x->file);
            if (ret) goto done;
            e->truncated_bytes += x->length - last * DE_PAGE;
            x->length = last * DE_PAGE;
        }
    }
    e->generation++; e->flushes++;
done:
    e->flushing = false;
    return de_error(e, ret);
}
static void de_native_header(struct de_engine *e, unsigned int part, db_u8 *b)
{
    struct de_extent *x = &e->extents[part];
    memset(b, 0, DE_PAGE); memcpy(b, "DBSPRS01", 8);
    de_put32(b + 8, 1); de_put32(b + 12, part); memcpy(b + 16, e->uuid, 16);
    de_put64(b + 32, e->capacity); de_put64(b + 40, e->span);
    de_put64(b + 48, e->part_limit); de_put32(b + 56, DE_GRAIN);
    de_put32(b + 60, 16); memcpy(b + 64, e->codec, 16);
    de_put64(b + 80, x->bitmap_start); de_put64(b + 88, x->bitmap_bytes);
    de_put64(b + 96, x->gd); de_put64(b + 104, x->table_start);
    de_put64(b + 112, x->data_start); de_put64(b + 120, e->generation);
    de_put32(b + DE_HEADER_CRC, de_crc(b, DE_HEADER_CRC));
}
static bool de_header_valid(const db_u8 *b)
{
    return !memcmp(b, "DBSPRS01", 8) && de_get32(b + 8) == 1 &&
        de_get32(b + DE_HEADER_CRC) == de_crc(b, DE_HEADER_CRC);
}
static int de_header_read(struct de_engine *e, unsigned int file)
{
    int ret = de_io(e, file, e->page, DE_PAGE, 0, false);
    int other = de_io(e, file, e->descriptor, DE_PAGE, DE_PAGE, false);
    bool a = !ret && de_header_valid(e->page);
    bool b = !other && de_header_valid(e->descriptor);
    if (!a && !b) return ret ? ret : other ? other : -EUCLEAN;
    if (!a || (b && de_get64(e->descriptor + 120) > de_get64(e->page + 120)))
        memcpy(e->page, e->descriptor, DE_PAGE);
    return 0;
}
static int de_native_geometry(struct de_engine *e, unsigned int part)
{
    struct de_extent *x = &e->extents[part];
    x->file = part; x->start = (db_u64)part * e->span; x->capacity = e->span;
    x->entries = 256; x->entry_bytes = 16; x->limit = e->part_limit;
    x->directories = (unsigned int)(de_round(e->span, DE_GRAIN * 256ULL) / (DE_GRAIN * 256ULL));
    x->bitmap_start = 2 * DE_PAGE;
    x->bitmap_bytes = de_round(de_round(x->limit, DE_PAGE * 8ULL) / (DE_PAGE * 8ULL), DE_PAGE);
    x->gd = x->bitmap_start + x->bitmap_bytes;
    x->table_start = x->gd + DE_PAGE;
    x->data_start = x->table_start + (db_u64)x->directories * DE_PAGE;
    x->hint = x->data_start / DE_PAGE;
    if (x->data_start + e->span > x->limit || x->directories > DE_PAGE / 4) return -EINVAL;
    x->directory = e->io.alloc(e->io.ctx, x->directories * 4);
    if (!x->directory) return -ENOMEM;
    return 0;
}
static int de_native_part(struct de_engine *e, unsigned int part, bool create)
{
    struct de_extent *x = &e->extents[part];
    unsigned int j;
    int ret = de_native_geometry(e, part);
    if (ret) return ret;
    if (part == 0) snprintf(x->name, sizeof(x->name), "%s", e->primary);
    else if (!strcmp(e->primary, "volume000.db")) snprintf(x->name, sizeof(x->name), "volume%03u.db", part);
    else snprintf(x->name, sizeof(x->name), "%.*s.%03u", DE_NAME - 16, e->primary, part);
    if (part || create) {
        ret = e->io.open(e->io.ctx, part, x->name, create);
        if (ret) return ret;
    }
    for (j = 0; j < x->directories; j++) x->directory[j] = (x->table_start + (db_u64)j * DE_PAGE) / 512;
    if (create) {
        ret = e->io.resize(e->io.ctx, part, x->data_start);
        if (ret) return ret;
        de_native_header(e, part, e->page);
        ret = de_io(e, part, e->page, DE_PAGE, 0, true);
        if (!ret) ret = de_io(e, part, e->page, DE_PAGE, DE_PAGE, true);
        if (ret) return ret;
        memset(e->page, 0, DE_PAGE);
        for (j = 0; j < x->directories; j++) de_put32(e->page + j * 4, x->directory[j]);
        ret = de_io(e, part, e->page, DE_PAGE, x->gd, true);
        if (ret) return ret;
        x->length = x->data_start;
        return de_sync(e, part);
    }
    ret = e->io.size(e->io.ctx, part, &x->length);
    if (ret || x->length < x->data_start || x->length > x->limit) return ret ? ret : -EUCLEAN;
    ret = de_header_read(e, part);
    if (ret) return ret;
    if (de_get32(e->page + 12) != part || memcmp(e->page + 16, e->uuid, 16) ||
        de_get64(e->page + 40) != e->span || de_get64(e->page + 48) != e->part_limit ||
        de_get32(e->page + 56) != DE_GRAIN || de_get32(e->page + 60) != 16 ||
        memcmp(e->page + 64, e->codec, 16) || de_get64(e->page + 80) != x->bitmap_start ||
        de_get64(e->page + 88) != x->bitmap_bytes || de_get64(e->page + 96) != x->gd ||
        de_get64(e->page + 104) != x->table_start || de_get64(e->page + 112) != x->data_start ||
        !de_zero(e->page + 128, DE_HEADER_CRC - 128)) return -EUCLEAN;
    ret = de_io(e, part, e->page, DE_PAGE, x->gd, false);
    if (ret) return ret;
    for (j = 0; j < x->directories; j++)
        if (de_get32(e->page + j * 4) != x->directory[j]) return -EUCLEAN;
    return 0;
}
static int de_vmdk_part(struct de_engine *e, unsigned int part, bool create)
{
    struct de_extent *x = &e->extents[part];
    db_u64 sectors, desc_end, bytes;
    unsigned int i, j, copy;
    int ret;
    x->file = part + 1; x->entries = 512; x->entry_bytes = 4;
    x->directories = de_round(x->capacity, DE_GRAIN * 512ULL) / (DE_GRAIN * 512ULL);
    if (!x->capacity || x->capacity > DE_SPLIT || x->capacity % 512 || !x->directories) return -EINVAL;
    x->limit = DE_PART_LIMIT;
    ret = e->io.open(e->io.ctx, x->file, x->name, create);
    if (ret) return ret;
    x->directory = e->io.alloc(e->io.ctx, x->directories * 4);
    if (!x->directory) return -ENOMEM;
    if (create) {
        x->rgd = 21 * 512ULL;
        x->gd = x->rgd + 512 + x->directories * 2048ULL;
        x->data_start = de_round(x->gd + 512 + x->directories * 2048ULL, DE_GRAIN);
        x->length = x->data_start; x->flags = 3;
        memset(e->page, 0, DE_PAGE);
        de_put32(e->page, 0x564d444b); de_put32(e->page + 4, 1); de_put32(e->page + 8, 3);
        de_put64(e->page + 12, x->capacity / 512); de_put64(e->page + 20, 128);
        de_put64(e->page + 28, 1); de_put64(e->page + 36, 20);
        de_put32(e->page + 44, 512); de_put64(e->page + 48, x->rgd / 512);
        de_put64(e->page + 56, x->gd / 512); de_put64(e->page + 64, x->data_start / 512);
        e->page[73] = 10; e->page[74] = 32; e->page[75] = 13; e->page[76] = 10;
        ret = e->io.resize(e->io.ctx, x->file, x->data_start);
        if (!ret) ret = de_io(e, x->file, e->page, DE_PAGE, 0, true);
        if (ret) return ret;
    } else {
        ret = e->io.size(e->io.ctx, x->file, &x->length);
        if (ret) return ret;
        ret = de_io(e, x->file, e->page, 512, 0, false);
        if (ret) return ret;
        sectors = de_get64(e->page + 12); x->flags = de_get32(e->page + 8);
        if (de_get32(e->page) != 0x564d444b ||
            (de_get32(e->page + 4) != 1 && de_get32(e->page + 4) != 2) ||
            (x->flags & ~7U) || de_get64(e->page + 20) != 128 ||
            de_get32(e->page + 44) != 512 || sectors != x->capacity / 512 ||
            e->page[77] || e->page[78]) return -EOPNOTSUPP;
        /* Bounds are checked before multiplying attacker-controlled sectors. */
        if (de_get64(e->page + 48) > DE_PART_LIMIT / 512 ||
            de_get64(e->page + 56) > DE_PART_LIMIT / 512 ||
            de_get64(e->page + 64) > DE_PART_LIMIT / 512 ||
            de_get64(e->page + 28) > DE_PART_LIMIT / 512 ||
            de_get64(e->page + 36) > DE_PART_LIMIT / 512) return -EUCLEAN;
        x->rgd = (x->flags & 2) ? de_get64(e->page + 48) * 512 : 0;
        x->gd = de_get64(e->page + 56) * 512;
        x->data_start = de_get64(e->page + 64) * 512;
        desc_end = (de_get64(e->page + 28) + de_get64(e->page + 36)) * 512;
        if (x->data_start % DE_GRAIN || x->data_start < DE_PAGE ||
            x->data_start > x->length || x->length > x->limit ||
            x->gd < 512 || x->gd + x->directories * 4 > x->data_start ||
            (x->rgd && (x->rgd < 512 || x->rgd + x->directories * 4 > x->data_start)) ||
            desc_end > x->data_start) return -EUCLEAN;
    }
    if (x->rgd) {
        x->backup = e->io.alloc(e->io.ctx, x->directories * 4);
        if (!x->backup) return -ENOMEM;
    }
    for (copy = 0; copy < (x->backup ? 2U : 1U); copy++) {
        db_u64 gd = copy ? x->rgd : x->gd;
        db_u32 *dir = copy ? x->backup : x->directory;
        if (create) {
            memset(e->page, 0, DE_PAGE);
            for (i = 0; i < x->directories; i++) de_put32(e->page + i * 4, (gd + 512 + i * 2048ULL) / 512);
            ret = de_io(e, x->file, e->page, x->directories * 4, gd, true);
        } else ret = de_io(e, x->file, e->page, x->directories * 4, gd, false);
        if (ret) return ret;
        for (i = 0; i < x->directories; i++) {
            dir[i] = de_get32(e->page + i * 4);
            bytes = (db_u64)dir[i] * 512;
            if (bytes < 512 || bytes + 2048 > x->data_start || bytes % 512 ||
                (bytes < x->gd + x->directories * 4 && bytes + 2048 > x->gd) ||
                (x->rgd && bytes < x->rgd + x->directories * 4 && bytes + 2048 > x->rgd)) return -EUCLEAN;
            for (j = 0; j < i; j++)
                if (bytes < (db_u64)dir[j] * 512 + 2048 && bytes + 2048 > (db_u64)dir[j] * 512) return -EUCLEAN;
            if (copy) for (j = 0; j < x->directories; j++)
                if (bytes < (db_u64)x->directory[j] * 512 + 2048 && bytes + 2048 > (db_u64)x->directory[j] * 512) return -EUCLEAN;
        }
    }
    x->next = de_round(x->length, DE_GRAIN);
    return create ? de_sync(e, x->file) : 0;
}
static int de_descriptor_reserve(struct de_engine *e)
{
    size_t need = 512;
    unsigned int i;
    for (i = 0; i < e->nr_extents; i++) need += strlen(e->extents[i].name) + 48;
    return de_reserve_descriptor(e, need);
}
static int de_descriptor_write(struct de_engine *e)
{
    size_t used;
    unsigned int i;
    int n, ret = de_descriptor_reserve(e);
    if (ret) return ret;
    n = snprintf((char *)e->descriptor, e->descriptor_bytes,
        "# Disk DescriptorFile\nversion=1\nCID=%08x\nparentCID=ffffffff\n"
        "createType=\"twoGbMaxExtentSparse\"\n\n# Extent description\n", de_crc(e->uuid, 16) ^ (db_u32)e->generation);
    if (n < 0 || (unsigned int)n >= e->descriptor_bytes) return -E2BIG;
    used = n;
    for (i = 0; i < e->nr_extents; i++) {
        struct de_extent *x = &e->extents[i];
        n = snprintf((char *)e->descriptor + used, e->descriptor_bytes - used,
                     "RW %llu SPARSE \"%s\"\n", (unsigned long long)(x->capacity / 512), x->name);
        if (n < 0 || (size_t)n >= e->descriptor_bytes - used) return -E2BIG;
        used += n;
    }
    n = snprintf((char *)e->descriptor + used, e->descriptor_bytes - used,
        "\n# The Disk Data Base\n#DDB\nddb.virtualHWVersion = \"4\"\n"
        "ddb.adapterType = \"lsilogic\"\nddb.geometry.cylinders = \"%llu\"\n"
        "ddb.geometry.heads = \"16\"\nddb.geometry.sectors = \"63\"\n",
        (unsigned long long)(e->capacity / (512 * 16 * 63)));
    if (n < 0 || (size_t)n >= e->descriptor_bytes - used) return -E2BIG;
    used += n;
    ret = de_io(e, 0, e->descriptor, used, 0, true);
    if (!ret) ret = e->io.resize(e->io.ctx, 0, used);
    if (!ret) ret = de_sync(e, 0);
    return ret;
}
static char *de_trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) *--end = 0;
    return s;
}
static int de_parse_vmdk_descriptor(struct de_engine *e, char *text, db_u64 length)
{
    char *line, *cursor, *eq, *key, *value;
    unsigned int seen = 0, i, j;
    size_t cid_offset = 0, cid_length = 0;
    int ret;
    cursor = text;
    while ((line = strsep(&cursor, "\n")) != NULL) {
        unsigned long long sectors;
        int consumed = 0;
        line = de_trim(line);
        if (!*line || *line == '#') continue;
        if (!strncmp(line, "RW ", 3) || !strncmp(line, "RW\t", 3)) {
            struct de_extent *x;
            if (e->nr_extents == DE_MAX_PARTS) return -E2BIG;
            ret = de_reserve_extents(e, e->nr_extents + 1);
            if (ret) return ret;
            x = &e->extents[e->nr_extents];
            if (sscanf(line, "RW %llu SPARSE \"%127[^\"]\" %n", &sectors, x->name, &consumed) != 2 ||
                !consumed || *de_trim(line + consumed) || !de_name_ok(x->name) ||
                !strcmp(x->name, e->primary) || !sectors || sectors > DE_SPLIT / 512) return -EOPNOTSUPP;
            for (j = 0; j < e->nr_extents; j++) if (!strcmp(e->extents[j].name, x->name)) return -EUCLEAN;
            x->start = e->capacity; x->capacity = sectors * 512;
            if (x->capacity > DE_LIMIT - e->capacity) return -EFBIG;
            e->capacity += x->capacity; e->nr_extents++;
            continue;
        }
        eq = strchr(line, '=');
        if (!eq) return -EOPNOTSUPP;
        *eq = 0; key = de_trim(line); value = de_trim(eq + 1);
        if (*value == '"') {
            size_t n = strlen(value);
            if (n < 2 || value[n - 1] != '"') return -EUCLEAN;
            value[n - 1] = 0; value++;
        }
        if (!strcmp(key, "version")) {
            if (seen & 1 || strcmp(value, "1")) return -EOPNOTSUPP;
            seen |= 1;
        } else if (!strcmp(key, "parentCID")) {
            if (seen & 2 || strcasecmp(value, "ffffffff")) return -EOPNOTSUPP;
            seen |= 2;
        } else if (!strcmp(key, "createType")) {
            if (seen & 4 || strcmp(value, "twoGbMaxExtentSparse")) return -EOPNOTSUPP;
            seen |= 4;
        } else if (!strcmp(key, "CID")) {
            /* QEMU prints PRIx32, so leading zeroes are not mandatory. */
            cid_length = strlen(value);
            if (seen & 8 || !cid_length || cid_length > 8) return -EUCLEAN;
            for (i = 0; i < cid_length; i++) if (!((value[i] >= '0' && value[i] <= '9') ||
                (value[i] >= 'a' && value[i] <= 'f') || (value[i] >= 'A' && value[i] <= 'F'))) return -EUCLEAN;
            cid_offset = value - text; seen |= 8;
        } else if (strncmp(key, "ddb.", 4)) return -EOPNOTSUPP;
    }
    if (seen != 15 || !e->nr_extents || e->capacity % 512) return -EUCLEAN;
    for (i = 0; i < e->nr_extents; i++) {
        ret = de_vmdk_part(e, i, false);
        if (ret) return ret;
    }
    if (!e->readonly) {
        char cid[9];
        snprintf(cid, sizeof(cid), "%08x", de_crc(e->descriptor, length) ^ de_crc(e->uuid, 16) ^ 0xa53127deU);
        if (cid_length < 8) {
            /* Expand the field without overwriting its quote/newline or the
             * next directive. Keep all caller-supplied descriptor fields. */
            size_t extra = 8 - cid_length;
            if (length + extra >= e->descriptor_bytes) return -E2BIG;
            memmove(e->descriptor + cid_offset + 8,
                    e->descriptor + cid_offset + cid_length,
                    (size_t)length - cid_offset - cid_length);
            memcpy(e->descriptor + cid_offset, cid, 8);
            length += extra;
            ret = de_io(e, 0, e->descriptor, (size_t)length, 0, true);
            if (!ret) ret = e->io.resize(e->io.ctx, 0, length);
        } else ret = de_io(e, 0, cid, 8, cid_offset, true);
        if (!ret) ret = de_sync(e, 0);
        if (ret) return ret;
    }
    return 0;
}
static int de_vmdk_descriptor(struct de_engine *e, db_u64 length)
{
    char *text;
    db_u8 *nul;
    int ret;
    if (!length || length >= DE_DESCRIPTOR_MAX) return -E2BIG;
    /* Include headroom for normalization of a short CID without a second realloc. */
    ret = de_reserve_descriptor(e, (size_t)length + 1 < DE_DESCRIPTOR_MAX - 8 ?
        (size_t)length + 9 : DE_DESCRIPTOR_MAX);
    if (ret) return ret;
    ret = de_io(e, 0, e->descriptor, (size_t)length, 0, false);
    if (ret) return ret;
    nul = memchr(e->descriptor, 0, (size_t)length);
    if (nul) length = nul - e->descriptor;
    e->descriptor[length] = 0;
    text = e->io.alloc(e->io.ctx, (size_t)length + 1);
    if (!text) return -ENOMEM;
    memcpy(text, e->descriptor, (size_t)length + 1);
    ret = de_parse_vmdk_descriptor(e, text, length);
    e->io.free(e->io.ctx, text);
    return ret;
}
static int de_value(struct de_engine *e, struct de_extent *x, const db_u8 value[16],
                    db_u64 *off, unsigned int *length)
{
    if (e->format == DE_NATIVE) {
        *off = de_get64(value); *length = de_get32(value + 8);
        if (!*off) return de_zero(value, 16) ? 0 : -EUCLEAN;
        if (!*length || *length > DE_GRAIN || *off % DE_PAGE ||
            (*length < DE_GRAIN && !strcmp(e->codec, "none")) ||
            (*length == DE_GRAIN && de_get32(value + 12))) return -EUCLEAN;
    } else {
        db_u32 sector = de_get32(value);
        if (sector == 1 && x->flags & 4) sector = 0;
        *off = (db_u64)sector * 512; *length = sector ? DE_GRAIN : 0;
        if (!sector) return 0;
        if (*off % DE_GRAIN) return -EUCLEAN;
    }
    if (*off < x->data_start || *off > x->limit - de_round(*length, DE_PAGE) ||
        *off > x->length || de_round(*length, DE_PAGE) > x->length - *off) return -EUCLEAN;
    return 0;
}
static int de_decode(struct de_engine *e, unsigned int part, const db_u8 value[16], db_u8 *out)
{
    struct de_extent *x = &e->extents[part];
    db_u64 off;
    unsigned int length, decoded = DE_GRAIN;
    int ret = de_value(e, x, value, &off, &length);
    if (ret) return ret;
    if (!off) { memset(out, 0, DE_GRAIN); return 0; }
    if (length == DE_GRAIN) return de_io(e, x->file, out, DE_GRAIN, off, false);
    ret = de_io(e, x->file, e->encoded, de_round(length, DE_PAGE), off, false);
    if (ret) return ret;
    if (de_crc(e->encoded, length) != de_get32(value + 12)) return -EUCLEAN;
    ret = e->io.codec(e->io.ctx, e->codec, false, e->encoded, length, out, &decoded);
    if (ret) return ret;
    return decoded == DE_GRAIN ? 0 : -EUCLEAN;
}
static int de_scan(struct de_engine *e, bool payload, bool rebuild)
{
    unsigned int part, i;
    int ret = 0;
    e->mapped_grains = e->stored_bytes = 0;
    for (part = 0; part < e->nr_extents; part++) {
        struct de_extent *x = &e->extents[part];
        db_u64 grain, off, page, pages;
        unsigned int length;
        db_u8 value[16], *bitmap;
        size_t bitmap_bytes = e->format == DE_NATIVE ? x->bitmap_bytes :
            (size_t)de_round(de_round(x->length, DE_PAGE * 8ULL) / (DE_PAGE * 8ULL), DE_PAGE);
        bitmap = e->io.alloc(e->io.ctx, bitmap_bytes);
        if (!bitmap) return -ENOMEM;
        memset(bitmap, 0, bitmap_bytes);
        for (page = 0; page < x->data_start / DE_PAGE; page++) bitmap[page / 8] |= 1U << (page % 8);
        for (grain = 0; grain < de_round(x->capacity, DE_GRAIN) / DE_GRAIN; grain++) {
            ret = de_map(e, part, grain, value, false);
            if (!ret) ret = de_value(e, x, value, &off, &length);
            if (ret) break;
            if (!off) continue;
            if (x->start + grain * DE_GRAIN >= e->capacity) { ret = -EUCLEAN; break; }
            page = off / DE_PAGE; pages = de_round(length, DE_PAGE) / DE_PAGE;
            for (i = 0; i < pages; i++) {
                if (bitmap[(page + i) / 8] & (1U << ((page + i) % 8))) { ret = -EUCLEAN; break; }
                bitmap[(page + i) / 8] |= 1U << ((page + i) % 8);
            }
            if (ret) break;
            e->mapped_grains++; e->stored_bytes += length;
            if (payload) {
                ret = de_decode(e, part, value, e->raw);
                if (ret) break;
            }
        }
        if (!ret && rebuild && e->format == DE_NATIVE) {
            ret = de_io(e, x->file, bitmap, bitmap_bytes, x->bitmap_start, true);
            for (i = 0; i < e->nr_cache; i++)
                if (e->cache[i].valid && e->cache[i].part == part && e->cache[i].kind == DE_BITMAP)
                    e->cache[i].valid = false;
        }
        if (!ret && e->format == DE_VMDK) {
            x->hint = x->data_start / DE_PAGE;
            while (x->hint < x->next / DE_PAGE && de_used(bitmap, x->hint))
                x->hint += DE_GRAIN / DE_PAGE;
        }
        e->io.free(e->io.ctx, bitmap);
        if (ret) return ret;
    }
    e->usage_part = ~0U;
    return 0;
}
int de_check(struct de_engine *e, bool payload)
{
    if (!e->readonly) return -EROFS;
    return de_scan(e, payload, false);
}
static bool de_codec_valid(const char *name)
{
    static const char *const names[] = {"none", "lz4", "lz4hc", "lzo", "lzo-rle", "zstd", "deflate", "842"};
    unsigned int i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) if (!strcmp(name, names[i])) return true;
    return false;
}
int de_create(struct de_engine *e, const char *name, unsigned int format, db_u64 capacity,
              db_u64 limit, const char *codec, const db_u8 uuid[16])
{
    unsigned int i;
    int ret;
    if (e->readonly) return -EROFS;
    if (!de_name_ok(name) || strlen(name) > DE_NAME - 16 || !de_codec_valid(codec) ||
        !capacity || capacity > DE_LIMIT || capacity % DE_PAGE ||
        (format != DE_NATIVE && format != DE_VMDK)) return -EINVAL;
    if (format == DE_VMDK && strcmp(codec, "none")) return -EOPNOTSUPP;
    e->format = format; e->capacity = capacity; e->generation = 1;
    memcpy(e->uuid, uuid, 16); strcpy(e->codec, codec); strcpy(e->primary, name);
    if (format == DE_NATIVE) {
        e->part_limit = limit ? limit : DE_PART_LIMIT;
        if (e->part_limit < (1ULL << 20) || e->part_limit > DE_PART_LIMIT || e->part_limit % DE_PAGE) return -EINVAL;
        e->span = de_native_span(e->part_limit);
    } else { e->span = DE_SPLIT; e->part_limit = DE_PART_LIMIT; }
    if (de_round(capacity, e->span) / e->span > DE_MAX_PARTS) return -EFBIG;
    e->nr_extents = de_round(capacity, e->span) / e->span;
    ret = de_reserve_extents(e, e->nr_extents);
    if (ret) return ret;
    if (format == DE_VMDK) {
        ret = de_reserve_descriptor(e, 512 + (size_t)e->nr_extents * (strlen(name) + 64));
        if (ret) return ret;
        ret = e->io.open(e->io.ctx, 0, name, true);
        if (ret) return ret;
    }
    for (i = 0; i < e->nr_extents; i++) {
        if (format == DE_NATIVE) ret = de_native_part(e, i, true);
        else {
            struct de_extent *x = &e->extents[i];
            size_t n = strlen(name);
            if (n > 5 && !strcmp(name + n - 5, ".vmdk")) n -= 5;
            snprintf(x->name, sizeof(x->name), "%.*s-s%03u.vmdk", (int)n, name, i + 1);
            x->start = (db_u64)i * DE_SPLIT;
            x->capacity = capacity - x->start < DE_SPLIT ? capacity - x->start : DE_SPLIT;
            ret = de_vmdk_part(e, i, true);
        }
        if (ret) return ret;
    }
    ret = e->io.sync_dir(e->io.ctx);
    if (!ret && format == DE_VMDK) ret = de_descriptor_write(e);
    if (!ret) ret = de_scan(e, false, true);
    if (!ret) e->initialized = true;
    return ret;
}
int de_open(struct de_engine *e, const char *name, unsigned int format)
{
    db_u64 length;
    unsigned int i;
    int ret;
    if (!de_name_ok(name) || strlen(name) > DE_NAME - 16 || format > DE_VMDK) return -EINVAL;
    strcpy(e->primary, name);
    ret = e->io.open(e->io.ctx, 0, name, false);
    if (!ret) ret = e->io.size(e->io.ctx, 0, &length);
    if (ret) return ret;
    if (length < 8) return -EUCLEAN;
    ret = de_io(e, 0, e->page, 8, 0, false);
    if (ret) return ret;
    if (!memcmp(e->page, "DBPART01", 8)) return -EPROTONOSUPPORT;
    if (length >= 2 * DE_PAGE && !de_header_read(e, 0)) {
        if (format == DE_VMDK) return -EINVAL;
        e->format = DE_NATIVE;
        e->capacity = de_get64(e->page + 32); e->span = de_get64(e->page + 40);
        e->part_limit = de_get64(e->page + 48); e->generation = de_get64(e->page + 120);
        memcpy(e->uuid, e->page + 16, 16); memcpy(e->codec, e->page + 64, 16);
        if (!memchr(e->codec, 0, 16) || !de_codec_valid(e->codec) || de_zero(e->uuid, 16) ||
            !e->generation || !e->capacity || e->capacity > DE_LIMIT || e->capacity % DE_PAGE ||
            e->span < DE_GRAIN || e->span > (1ULL << 30) || e->span % DE_GRAIN ||
            e->part_limit < (1ULL << 20) || e->part_limit > DE_PART_LIMIT || e->part_limit % DE_PAGE ||
            de_round(e->capacity, e->span) / e->span > DE_MAX_PARTS) return -EUCLEAN;
        e->nr_extents = de_round(e->capacity, e->span) / e->span;
        ret = de_reserve_extents(e, e->nr_extents);
        if (ret) return ret;
        for (i = 0; i < e->nr_extents; i++) {
            ret = de_native_part(e, i, false);
            if (ret) return ret;
        }
    } else {
        if (format == DE_NATIVE) return -EUCLEAN;
        e->format = DE_VMDK; e->span = DE_SPLIT; e->part_limit = DE_PART_LIMIT;
        strcpy(e->codec, "none"); e->generation = 1;
        ret = de_vmdk_descriptor(e, length);
        if (ret) return ret;
    }
    ret = de_scan(e, false, !e->readonly);
    if (!ret) e->initialized = true;
    return ret;
}
static int de_find(struct de_engine *e, db_u64 pos, unsigned int *part, db_u64 *grain, unsigned int *within)
{
    unsigned int lo = 0, hi = e->nr_extents;
    while (lo < hi) {
        unsigned int mid = lo + (hi - lo) / 2;
        struct de_extent *x = &e->extents[mid];
        if (pos < x->start) hi = mid;
        else if (pos - x->start >= x->capacity) lo = mid + 1;
        else { *part = mid; *grain = (pos - x->start) / DE_GRAIN; *within = (pos - x->start) % DE_GRAIN; return 0; }
    }
    return -EIO;
}
/* Reconstruct only the active part's physical usage. The bitmap is bounded
 * by the per-file cap (128 KiB at 4000 MiB), independent of virtual capacity.
 * It includes retired-but-not-yet-durable references; ordinary map changes
 * and retirement keep a resident bitmap coherent between reclaim steps. */
static int de_usage(struct de_engine *e, unsigned int part)
{
    struct de_extent *x = &e->extents[part];
    db_u64 grain, off;
    unsigned int length, i;
    db_u8 value[16];
    int ret;
    if (e->usage_part == part) return 0;
    if (!e->usage_bitmap) {
        e->usage_bytes = de_round(de_round(DE_PART_LIMIT, DE_PAGE * 8ULL) / (DE_PAGE * 8ULL), DE_PAGE);
        e->usage_bitmap = e->io.alloc(e->io.ctx, e->usage_bytes);
        if (!e->usage_bitmap) { e->usage_bytes = 0; return -ENOMEM; }
    }
    e->usage_part = ~0U;
    memset(e->usage_bitmap, 0, e->usage_bytes);
    de_mark(e->usage_bitmap, 0, x->data_start / DE_PAGE, true);
    for (grain = 0; grain < de_round(x->capacity, DE_GRAIN) / DE_GRAIN; grain++) {
        ret = de_map(e, part, grain, value, false);
        if (!ret) ret = de_value(e, x, value, &off, &length);
        if (ret) return ret;
        if (off) de_mark(e->usage_bitmap, off / DE_PAGE, de_round(length, DE_PAGE) / DE_PAGE, true);
    }
    for (i = 0; i < e->nr_retired; i++) if (e->retired[i].part == part)
        de_mark(e->usage_bitmap, e->retired[i].page, e->retired[i].count, true);
    e->usage_part = part;
    return 0;
}
static int de_free_run(struct de_engine *e, unsigned int part, unsigned int count,
                        db_u64 before, db_u64 from, db_u64 *out)
{
    struct de_extent *x = &e->extents[part];
    db_u64 page;
    unsigned int run = 0;
    int ret = de_usage(e, part);
    if (ret) return ret;
    if (from < x->data_start / DE_PAGE) from = x->data_start / DE_PAGE;
    for (page = from; page < before / DE_PAGE; page++) {
        if (!run && e->format == DE_VMDK && page % (DE_GRAIN / DE_PAGE)) continue;
        run = de_used(e->usage_bitmap, page) ? 0 : run + 1;
        if (run == count) { *out = (page + 1 - run) * DE_PAGE; return 0; }
    }
    return -ENOSPC;
}
static int de_vmdk_allocate(struct de_engine *e, unsigned int part, db_u64 *off)
{
    struct de_extent *x = &e->extents[part];
    int ret = -ENOSPC;
    if (x->hint < x->next / DE_PAGE)
        ret = de_free_run(e, part, DE_GRAIN / DE_PAGE, x->next, x->hint, off);
    if (ret == -ENOSPC) {
        *off = x->next;
        if (*off > x->limit - DE_GRAIN) return -ENOSPC;
        x->next += DE_GRAIN;
    } else if (ret) return ret;
    x->hint = (*off + DE_GRAIN) / DE_PAGE;
    de_usage_mark(e, part, *off / DE_PAGE, DE_GRAIN / DE_PAGE, true);
    return 0;
}
static int de_retire(struct de_engine *e, unsigned int part, db_u64 off, unsigned int length)
{
    struct de_retired *r;
    if (!off) return 0;
    if (e->nr_retired >= DE_RETIRED) return -EUCLEAN;
    r = &e->retired[e->nr_retired++];
    r->part = part; r->page = off / DE_PAGE; r->count = de_round(length, DE_PAGE) / DE_PAGE;
    e->extents[part].trim = true;
    return 0;
}
static int de_unmap_grain(struct de_engine *e, unsigned int part, db_u64 grain)
{
    db_u8 old[16], empty[16] = {0};
    db_u64 off;
    unsigned int length;
    int ret;
    if (e->nr_retired == DE_RETIRED && (ret = de_flush(e))) return ret;
    ret = de_map(e, part, grain, old, false);
    if (!ret) ret = de_value(e, &e->extents[part], old, &off, &length);
    if (ret || !off) return ret;
    ret = de_map(e, part, grain, empty, true);
    if (!ret) ret = de_retire(e, part, off, length);
    if (!ret) { e->mapped_grains--; e->stored_bytes -= length; }
    return ret;
}
static int de_allocate(struct de_engine *e, unsigned int part, unsigned int count, db_u64 *off)
{
    struct de_extent *x = &e->extents[part];
    db_u64 page, start;
    unsigned int pass, run, j;
    int ret;
    bool used;
    for (pass = 0; pass < 2; pass++) {
        start = pass ? x->data_start / DE_PAGE : x->hint;
        run = 0;
        for (page = start; page < x->limit / DE_PAGE; page++) {
            ret = de_bitmap(e, part, page, -1, &used);
            if (ret) return ret;
            run = used ? 0 : run + 1;
            if (run != count) continue;
            start = page + 1 - count;
            for (j = 0; j < count; j++) {
                ret = de_bitmap(e, part, start + j, 1, NULL);
                if (ret) return ret;
            }
            x->hint = page + 1; *off = start * DE_PAGE;
            de_usage_mark(e, part, start, count, true);
            return 0;
        }
    }
    return -ENOSPC;
}
int de_read(struct de_engine *e, db_u64 pos, void *out, unsigned int bytes)
{
    db_u8 *dst = out, value[16];
    db_u64 grain, off;
    unsigned int part, within, n, length;
    int ret;
    if (pos > e->capacity || bytes > e->capacity - pos || pos % 512 || bytes % 512) return -EINVAL;
    while (bytes) {
        ret = de_find(e, pos, &part, &grain, &within);
        if (ret) return ret;
        n = bytes < DE_GRAIN - within ? bytes : DE_GRAIN - within;
        if (n > e->extents[part].capacity - (pos - e->extents[part].start))
            n = e->extents[part].capacity - (pos - e->extents[part].start);
        ret = de_map(e, part, grain, value, false);
        if (!ret) ret = de_value(e, &e->extents[part], value, &off, &length);
        if (ret) return de_error(e, ret);
        if (!off) memset(dst, 0, n);
        else if (length == DE_GRAIN) ret = de_io(e, e->extents[part].file, dst, n, off + within, false);
        else {
            ret = de_decode(e, part, value, e->raw);
            if (!ret) memcpy(dst, e->raw + within, n);
        }
        if (ret) return de_error(e, ret);
        dst += n; pos += n; bytes -= n;
    }
    return 0;
}
static int de_write_grain(struct de_engine *e, unsigned int part, db_u64 grain,
                          unsigned int within, const void *input, unsigned int n)
{
    struct de_extent *x = &e->extents[part];
    db_u8 old[16], value[16] = {0};
    db_u64 off, location = 0;
    unsigned int length, encoded = DE_GRAIN * 2, padded;
    void *data = e->raw;
    int ret;
    if (e->nr_retired == DE_RETIRED) {
        ret = de_flush(e);
        if (ret) return ret;
    }
    ret = de_map(e, part, grain, old, false);
    if (!ret) ret = de_value(e, x, old, &off, &length);
    if (ret) return ret;
    /* Ordinary raw overwrites do not change mappings or allocate storage.
     * Native full-grain writes may recompress or release an all-zero grain. */
    if (off && length == DE_GRAIN && (e->format == DE_VMDK || n < DE_GRAIN)) {
        ret = de_io(e, x->file, (void *)input, n, off + within, true);
        if (!ret) x->dirty_data = true;
        return ret;
    }
    if (within || n < DE_GRAIN) {
        ret = de_decode(e, part, old, e->raw);
        if (ret) return ret;
    }
    memcpy(e->raw + within, input, n);
    if (de_zero(e->raw, DE_GRAIN)) encoded = 0;
    else if (e->format == DE_NATIVE && strcmp(e->codec, "none")) {
        ret = e->io.codec(e->io.ctx, e->codec, true, e->raw, DE_GRAIN, e->encoded, &encoded);
        if (ret) return ret;
        if (!encoded) return -EIO;
        if (de_round(encoded, DE_PAGE) < DE_GRAIN) data = e->encoded;
        else encoded = DE_GRAIN;
    } else encoded = DE_GRAIN;
    if (encoded) {
        padded = de_round(encoded, DE_PAGE);
        if (off && length == DE_GRAIN && encoded == DE_GRAIN) {
            ret = de_io(e, x->file, data, DE_GRAIN, off, true);
            if (!ret) x->dirty_data = true;
            return ret;
        }
        if (e->format == DE_NATIVE) {
            ret = de_allocate(e, part, padded / DE_PAGE, &location);
            if (ret == -ENOSPC && e->nr_retired) {
                ret = de_flush(e);
                if (!ret) ret = de_allocate(e, part, padded / DE_PAGE, &location);
            }
            if (ret) return ret;
        } else {
            ret = de_vmdk_allocate(e, part, &location);
            if (ret) return ret;
        }
        if (padded > encoded) memset((db_u8 *)data + encoded, 0, padded - encoded);
        ret = de_io(e, x->file, data, padded, location, true);
        if (ret) return ret;
        x->dirty_data = true;
        if (x->length < location + padded) x->length = location + padded;
        if (e->format == DE_NATIVE) {
            de_put64(value, location); de_put32(value + 8, encoded);
            if (encoded < DE_GRAIN) de_put32(value + 12, de_crc(data, encoded));
        } else de_put32(value, location / 512);
    }
    if (!off && !location) return 0;
    ret = de_map(e, part, grain, value, true);
    if (ret) return ret;
    if (off) {
        ret = de_retire(e, part, off, length);
        if (ret) return ret;
    }
    e->mapped_grains += !!location; e->mapped_grains -= !!off;
    e->stored_bytes += encoded; e->stored_bytes -= length;
    return 0;
}
int de_write(struct de_engine *e, db_u64 pos, const void *input, unsigned int bytes)
{
    const db_u8 *src = input;
    db_u64 grain;
    unsigned int part, within, n;
    int ret;
    if (e->readonly) return -EROFS;
    if (e->failed) return -EIO;
    if (pos > e->capacity || bytes > e->capacity - pos || pos % 512 || bytes % 512) return -EINVAL;
    while (bytes) {
        ret = de_find(e, pos, &part, &grain, &within);
        if (ret) return ret;
        n = bytes < DE_GRAIN - within ? bytes : DE_GRAIN - within;
        if (n > e->extents[part].capacity - (pos - e->extents[part].start))
            n = e->extents[part].capacity - (pos - e->extents[part].start);
        ret = de_write_grain(e, part, grain, within, src, n);
        if (ret) return de_error(e, ret);
        src += n; bytes -= n; pos += n;
    }
    return (e->cache_mode == DE_WRITETHROUGH || e->cache_mode == DE_DIRECTSYNC) ? de_flush(e) : 0;
}
int de_discard(struct de_engine *e, db_u64 pos, unsigned int bytes)
{
    db_u8 *zeros;
    db_u64 grain;
    unsigned int n, part, within;
    int ret = 0;
    if (e->readonly) return -EROFS;
    if (e->failed) return -EIO;
    if (pos > e->capacity || bytes > e->capacity - pos || pos % 512 || bytes % 512) return -EINVAL;
    zeros = e->io.alloc(e->io.ctx, DE_GRAIN);
    if (!zeros) return -ENOMEM;
    memset(zeros, 0, DE_GRAIN);
    while (bytes && !ret) {
        ret = de_find(e, pos, &part, &grain, &within);
        if (ret) break;
        n = bytes < DE_GRAIN - within ? bytes : DE_GRAIN - within;
        if (n > e->extents[part].capacity - (pos - e->extents[part].start))
            n = e->extents[part].capacity - (pos - e->extents[part].start);
        if (!within && n == DE_GRAIN) ret = de_unmap_grain(e, part, grain);
        else ret = de_write(e, pos, zeros, n);
        pos += n; bytes -= n;
    }
    e->io.free(e->io.ctx, zeros);
    if (!ret && (e->cache_mode == DE_WRITETHROUGH || e->cache_mode == DE_DIRECTSYNC)) ret = de_flush(e);
    return de_error(e, ret);
}
int de_grow(struct de_engine *e, db_u64 capacity)
{
    db_u64 old_capacity = e->capacity, start, needed;
    unsigned int count, i, old_count = e->nr_extents;
    int ret;
    if (e->readonly) return -EROFS;
    if (e->failed) return -EIO;
    if (capacity < old_capacity || capacity > DE_LIMIT || capacity % DE_PAGE) return -EINVAL;
    if (capacity == old_capacity) return 0;
    needed = e->format == DE_NATIVE ? de_round(capacity, e->span) / e->span :
        (db_u64)old_count + de_round(capacity - old_capacity, DE_SPLIT) / DE_SPLIT;
    if (needed > DE_MAX_PARTS) return -EFBIG;
    count = (unsigned int)needed;
    ret = de_reserve_extents(e, count);
    if (ret) return ret;
    if (e->format == DE_VMDK) {
        ret = de_reserve_descriptor(e, 512 + (size_t)count * (strlen(e->primary) + 64));
        if (ret) return ret;
    }
    ret = de_flush(e);
    if (ret) return ret;
    e->capacity = capacity;
    start = old_capacity;
    for (i = old_count; i < count; i++) {
        if (e->format == DE_NATIVE) ret = de_native_part(e, i, true);
        else {
            struct de_extent *x = &e->extents[i];
            size_t n = strlen(e->primary);
            if (n > 5 && !strcmp(e->primary + n - 5, ".vmdk")) n -= 5;
            snprintf(x->name, sizeof(x->name), "%.*s-s%03u.vmdk", (int)n, e->primary, i + 1);
            x->start = start; x->capacity = capacity - start < DE_SPLIT ? capacity - start : DE_SPLIT;
            ret = de_vmdk_part(e, i, true);
            start += x->capacity;
        }
        if (ret) { e->capacity = old_capacity; return de_error(e, ret); }
        e->nr_extents = i + 1;
    }
    ret = e->io.sync_dir(e->io.ctx);
    if (ret) { e->capacity = old_capacity; return de_error(e, ret); }
    e->nr_extents = count;
    if (e->format == DE_NATIVE) {
        de_native_header(e, 0, e->page);
        ret = de_io(e, 0, e->page, DE_PAGE, 0, true);
        if (!ret) ret = de_sync(e, 0);
        if (!ret) ret = de_io(e, 0, e->page, DE_PAGE, DE_PAGE, true);
        if (!ret) ret = de_sync(e, 0);
        if (!ret) ret = de_scan(e, false, true);
    } else ret = de_descriptor_write(e);
    return de_error(e, ret);
}


/* Relocate an encoded grain without recompressing it. Source and destination
 * must not overlap: the old reference remains readable until flush succeeds. */
static int de_move_grain(struct de_engine *e, unsigned int part, db_u64 grain,
                          db_u8 value[16], db_u64 old, unsigned int length, db_u64 target)
{
    struct de_extent *x = &e->extents[part];
    unsigned int bytes = de_round(length, DE_PAGE), i;
    int ret;
    if (target >= old || target + bytes > old || target < x->data_start) return -EINVAL;
    if (e->nr_retired == DE_RETIRED && (ret = de_flush(e))) return ret;
    if (e->format == DE_NATIVE) for (i = 0; i < bytes / DE_PAGE; i++) {
        ret = de_bitmap(e, part, target / DE_PAGE + i, 1, NULL);
        if (ret) return ret;
    }
    de_usage_mark(e, part, target / DE_PAGE, bytes / DE_PAGE, true);
    ret = de_io(e, x->file, e->encoded, bytes, old, false);
    if (!ret) ret = de_io(e, x->file, e->encoded, bytes, target, true);
    if (ret) return ret;
    x->dirty_data = true;
    if (e->format == DE_NATIVE) de_put64(value, target);
    else de_put32(value, target / 512);
    ret = de_map(e, part, grain, value, true);
    if (!ret) ret = de_retire(e, part, old, length);
    return ret;
}
static int de_trim_usage(struct de_engine *e, unsigned int part)
{
    struct de_extent *x = &e->extents[part];
    db_u64 end = de_round(x->length, DE_PAGE) / DE_PAGE;
    int ret = de_usage(e, part);
    if (ret) return ret;
    while (end > x->data_start / DE_PAGE && !de_used(e->usage_bitmap, end - 1)) end--;
    if (e->format == DE_VMDK) end = de_round(end, DE_GRAIN / DE_PAGE);
    if (end * DE_PAGE < x->length) {
        ret = e->io.resize(e->io.ctx, x->file, end * DE_PAGE);
        if (!ret) ret = de_sync(e, x->file);
        if (ret) return ret;
        e->truncated_bytes += x->length - end * DE_PAGE;
        x->length = end * DE_PAGE;
    }
    if (e->format == DE_VMDK) x->next = de_round(x->length, DE_GRAIN);
    if (x->hint > end) x->hint = end;
    x->trim = false;
    return 0;
}
int de_reclaim_step(struct de_engine *e, struct de_reclaim *r)
{
    struct de_extent *x;
    db_u64 off, target, total, start, stop;
    db_u8 value[16];
    unsigned int length, scanned = 0, moved = 0;
    bool compact, punch;
    int ret;
    if (e->readonly) return -EROFS;
    if (e->failed) return -EIO;
    /* Never punch/truncate while deliberately ignoring ordering barriers. */
    if (e->cache_mode == DE_UNSAFE) return -EOPNOTSUPP;
    if (r->flags & ~(DE_RECLAIM_COMPACT | DE_RECLAIM_ZEROES) || r->phase > 1) return -EINVAL;
    if (r->part >= e->nr_extents) { r->done = 1; return 0; }
    x = &e->extents[r->part];
    total = de_round(x->capacity, DE_GRAIN) / DE_GRAIN;
    if ((!r->phase && r->cursor > total) ||
        (r->phase && r->cursor > x->limit / DE_PAGE)) return -EINVAL;
    ret = de_flush(e);
    if (!ret) ret = de_usage(e, r->part);
    if (ret) return de_error(e, ret);
    /* Relocation is NEVER an automatic fallback. On exFAT, ordinary reclaim
     * trims tails only; moving live data requires an explicit COMPACT flag. */
    compact = !!(r->flags & DE_RECLAIM_COMPACT);
    punch = e->io.punch && !e->punch_disabled;
    r->method = compact ? 2 : punch ? 1 : 3;
    if (!r->phase) {
        if (!compact && !(r->flags & DE_RECLAIM_ZEROES)) r->cursor = total;
        while (r->cursor < total && scanned < 1024 && moved < 4 * 1048576U) {
            db_u64 grain = r->cursor++;
            scanned++; r->scanned_bytes += DE_GRAIN;
            ret = de_map(e, r->part, grain, value, false);
            if (!ret) ret = de_value(e, x, value, &off, &length);
            if (ret) return de_error(e, ret);
            if (!off) continue;
            if (r->flags & DE_RECLAIM_ZEROES) {
                ret = de_decode(e, r->part, value, e->raw);
                if (ret) return de_error(e, ret);
                if (de_zero(e->raw, DE_GRAIN)) {
                    ret = de_unmap_grain(e, r->part, grain);
                    if (ret) return de_error(e, ret);
                    r->unmapped_bytes += DE_GRAIN;
                    continue;
                }
            }
            if (!compact) continue;
            ret = de_free_run(e, r->part, de_round(length, DE_PAGE) / DE_PAGE,
                              off, x->data_start / DE_PAGE, &target);
            if (ret == -ENOSPC) continue;
            if (!ret) ret = de_move_grain(e, r->part, grain, value, off, length, target);
            if (ret) return de_error(e, ret);
            moved += de_round(length, DE_PAGE); r->moved_bytes += de_round(length, DE_PAGE);
        }
        ret = de_flush(e);
        if (ret) return ret;
        if (r->cursor < total) return 0;
        r->phase = 1; r->cursor = x->data_start / DE_PAGE;
    }
    if (!compact && punch) {
        /* Bound the hole sweep too; no device-wide stop-the-world pass. */
        total = de_round(x->length, DE_PAGE) / DE_PAGE;
        stop = r->cursor + 16384;
        if (stop > total) stop = total;
        if (r->cursor < x->data_start / DE_PAGE) r->cursor = x->data_start / DE_PAGE;
        while (r->cursor < stop) {
            if (de_used(e->usage_bitmap, r->cursor)) { r->cursor++; continue; }
            start = r->cursor;
            do { r->cursor++; } while (r->cursor < stop && !de_used(e->usage_bitmap, r->cursor));
            ret = de_punch(e, r->part, start * DE_PAGE, (r->cursor - start) * DE_PAGE);
            if (ret) return de_error(e, ret);
            if (e->punch_disabled) { r->method = 3; break; }
        }
        ret = de_sync(e, x->file);
        if (ret) return de_error(e, ret);
        if (!e->punch_disabled && r->cursor < total) return 0;
    }
    ret = de_trim_usage(e, r->part);
    if (ret) return de_error(e, ret);
    r->part++; r->phase = 0; r->cursor = 0;
    r->done = r->part >= e->nr_extents;
    return 0;
}
