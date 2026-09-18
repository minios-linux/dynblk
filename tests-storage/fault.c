/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Power-cut model: file writes are volatile until the file's sync callback.
 * Inject an I/O error before every callback, then discard volatile state. */
#include "../dynblk_engine.h"
#include <assert.h>
#include <stdlib.h>
#define FILES 4
#define SPACE (2U * 1048576)
extern int LZ4_compress_default(const char *, char *, int, int);
extern int LZ4_decompress_safe(const char *, char *, int, int);
struct image { unsigned char *live, *stable; size_t size, durable; bool exists, named; };
struct model { struct image files[FILES]; unsigned int events, fail_at; };
static int fault(struct model *m) { return ++m->events == m->fail_at ? -EIO : 0; }
static void *alloc_cb(void *ctx, size_t n) { (void)ctx; return calloc(1, n); }
static void free_cb(void *ctx, void *p) { (void)ctx; free(p); }
static int open_cb(void *ctx, unsigned int id, const char *name, bool create)
{
    struct model *m = ctx;
    (void)name;
    if (id >= FILES) return -E2BIG;
    if (create && m->files[id].exists) return -EEXIST;
    if (!create && !m->files[id].exists) return -ENOENT;
    m->files[id].exists = true;
    return 0;
}
static int io_cb(void *ctx, unsigned int id, void *p, size_t n, db_u64 off, bool write)
{
    struct model *m = ctx;
    struct image *f;
    if (fault(m)) return -EIO;
    if (id >= FILES || off > SPACE || n > SPACE - off) return -E2BIG;
    f = &m->files[id];
    if (!f->exists) return -ENOENT;
    if (!write && (off > f->size || n > f->size - off)) return -EIO;
    if (write) {
        memcpy(f->live + off, p, n);
        if (f->size < off + n) f->size = off + n;
    } else memcpy(p, f->live + off, n);
    return 0;
}
static int size_cb(void *ctx, unsigned int id, db_u64 *size)
{
    struct model *m = ctx;
    if (id >= FILES || !m->files[id].exists) return -ENOENT;
    *size = m->files[id].size; return 0;
}
static int resize_cb(void *ctx, unsigned int id, db_u64 size)
{
    struct model *m = ctx;
    struct image *f;
    if (fault(m)) return -EIO;
    if (id >= FILES || size > SPACE) return -E2BIG;
    f = &m->files[id];
    if (size > f->size) memset(f->live + f->size, 0, size - f->size);
    f->size = size; return 0;
}
static int sync_cb(void *ctx, unsigned int id)
{
    struct model *m = ctx;
    struct image *f;
    if (fault(m)) return -EIO;
    if (id >= FILES) return -E2BIG;
    f = &m->files[id];
    memcpy(f->stable, f->live, f->size); f->durable = f->size;
    return 0;
}
static int dir_cb(void *ctx)
{
    struct model *m = ctx;
    unsigned int i;
    if (fault(m)) return -EIO;
    for (i = 0; i < FILES; i++) m->files[i].named = m->files[i].exists;
    return 0;
}
static int codec_cb(void *ctx, const char *name, bool encode, const void *src,
                     unsigned int bytes, void *dst, unsigned int *len)
{
    int n;
    (void)ctx;
    if (strcmp(name, "lz4")) return -EINVAL;
    n = encode ? LZ4_compress_default(src, dst, bytes, *len) : LZ4_decompress_safe(src, dst, bytes, *len);
    if (n <= 0) return -EIO;
    *len = n; return 0;
}
static void prepare(struct model *m)
{
    unsigned int i;
    memset(m, 0, sizeof(*m));
    for (i = 0; i < FILES; i++) {
        m->files[i].live = calloc(1, SPACE); m->files[i].stable = calloc(1, SPACE);
        assert(m->files[i].live && m->files[i].stable);
    }
}
static void finish(struct model *m)
{
    unsigned int i;
    for (i = 0; i < FILES; i++) { free(m->files[i].live); free(m->files[i].stable); }
}
static void clone(struct model *dst, const struct model *src)
{
    unsigned int i;
    for (i = 0; i < FILES; i++) {
        memcpy(dst->files[i].live, src->files[i].stable, SPACE);
        memcpy(dst->files[i].stable, src->files[i].stable, SPACE);
        dst->files[i].size = dst->files[i].durable = src->files[i].durable;
        dst->files[i].exists = dst->files[i].named = src->files[i].named;
    }
    dst->events = dst->fail_at = 0;
}
static struct de_io callbacks(struct model *m)
{
    return (struct de_io){.ctx=m, .alloc=alloc_cb, .free=free_cb, .open=open_cb,
        .io=io_cb, .size=size_cb, .resize=resize_cb, .sync=sync_cb,
        .sync_dir=dir_cb, .codec=codec_cb};
}
static int transaction(struct de_engine *e, unsigned char *buf, bool *committed)
{
    int ret;
    *committed = false;
    memset(buf, 'C', 4096); ret = de_write(e, 0, buf, 4096);
    if (!ret) ret = de_flush(e);
    if (ret) return ret;
    *committed = true;
    memset(buf, 'D', 4096); ret = de_write(e, 4096, buf, 4096);
    if (!ret) ret = de_flush(e);
    return ret;
}
int main(void)
{
    unsigned int format, cutoff, events, i, cases = 0;
    unsigned char *buf = malloc(DE_GRAIN), uuid[16] = {1};
    assert(buf);
    for (format = DE_NATIVE; format <= DE_VMDK; format++) {
        struct model baseline, trial;
        struct de_engine e = {0};
        struct de_io io;
        const char *name = format == DE_NATIVE ? "volume000.db" : "disk.vmdk";
        bool committed;
        int ret;
        prepare(&baseline); prepare(&trial); io = callbacks(&baseline);
        assert(!de_init(&e, &io, 1, false, DE_WRITEBACK));
        assert(!de_create(&e, name, format, 262144, format == DE_NATIVE ? 1048576 : 0,
                          format == DE_NATIVE ? "lz4" : "none", uuid));
        memset(buf, 'A', DE_GRAIN); assert(!de_write(&e, 0, buf, DE_GRAIN));
        memset(buf, 'B', DE_GRAIN); assert(!de_write(&e, DE_GRAIN, buf, DE_GRAIN));
        assert(!de_flush(&e)); de_destroy(&e);
        clone(&trial, &baseline); io = callbacks(&trial);
        assert(!de_init(&e, &io, 1, false, DE_WRITEBACK)); assert(!de_open(&e, name, format));
        trial.events = 0;
        assert(!transaction(&e, buf, &committed)); events = trial.events;
        de_destroy(&e);
        for (cutoff = 1; cutoff <= events + 1; cutoff++) {
            clone(&trial, &baseline); io = callbacks(&trial);
            assert(!de_init(&e, &io, 1, false, DE_WRITEBACK)); assert(!de_open(&e, name, format));
            trial.events = 0; trial.fail_at = cutoff;
            ret = transaction(&e, buf, &committed);
            assert(cutoff > events ? !ret : ret < 0);
            de_destroy(&e);
            /* A crash discards every non-synchronized byte and file-size change. */
            for (i = 0; i < FILES; i++) {
                memcpy(trial.files[i].live, trial.files[i].stable, SPACE);
                trial.files[i].size = trial.files[i].durable;
                trial.files[i].exists = trial.files[i].named;
            }
            trial.fail_at = 0;
            assert(!de_init(&e, &io, 1, true, DE_WRITEBACK)); assert(!de_open(&e, name, format));
            assert(!de_check(&e, true)); assert(!de_read(&e, 0, buf, DE_GRAIN));
            for (i = 0; i < DE_GRAIN; i++) {
                if (i < 4096) assert(committed ? buf[i] == 'C' : buf[i] == 'A' || buf[i] == 'C');
                else if (i < 8192) assert(buf[i] == 'A' || buf[i] == 'D');
                else assert(buf[i] == 'A');
            }
            assert(!de_read(&e, DE_GRAIN, buf, DE_GRAIN));
            for (i = 0; i < DE_GRAIN; i++) assert(buf[i] == 'B');
            de_destroy(&e); cases++;
        }
        printf("PASS %s: %u callback power-cut positions preserve completed flushes and untouched data\n",
               de_format_name(format), events + 1);
        finish(&baseline); finish(&trial);
    }
    free(buf);
    printf("PASS %u total power-cut model cases\n", cases);
    return 0;
}
