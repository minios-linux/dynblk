// SPDX-License-Identifier: GPL-2.0-or-later
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "dynblk_host.h"
#include "dynblk_common.h"
#include "lzo/decompress.h"
#include <stdlib.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#ifndef DYNBLK_NO_DYNAMIC_CODECS
#include <dlfcn.h>
#include <zlib.h>
#endif
#define DB_BLOCK DE_PAGE
#define DB_REGION DE_GRAIN
static int fail(char *error, size_t size, const char *format, ...)
{
	va_list args;
	if (error && size) {
		va_start(args, format);
		vsnprintf(error, size, format, args);
		va_end(args);
	}
	return -1;
}
#ifndef DYNBLK_NO_DYNAMIC_CODECS
static int load_symbol(void *handle, const char *codec, const char *name, void **symbol,
		       char *error, size_t error_size)
{
	const char *message;
	dlerror();
	*symbol = dlsym(handle, name);
	message = dlerror();
	if (message || !*symbol)
		return fail(error, error_size, "cannot validate %s: missing decoder symbol %s",
			    codec, name);
	return 0;
}

static void *load_decoder_library(const char *codec, const char *soname,
				  char *error, size_t error_size)
{
	void *handle = dlopen(soname, RTLD_NOW | RTLD_LOCAL);
	if (!handle)
		fail(error, error_size, "cannot validate %s: decoder library %s is unavailable",
		     codec, soname);
	return handle;
}

static int decode_lz4(const char *codec, const uint8_t *input, uint32_t input_length,
		      uint8_t *output, uint32_t expected, char *error, size_t error_size)
{
	typedef int (*decode_fn)(const char *, char *, int, int);
	static void *handle;
	static decode_fn decode;
	int length;
	if (!decode) {
		if (!handle)
			handle = load_decoder_library(codec, "liblz4.so.1", error, error_size);
		if (!handle || load_symbol(handle, codec, "LZ4_decompress_safe",
					   (void **)&decode, error, error_size))
			return -1;
	}
	length = decode((const char *)input, (char *)output, (int)input_length, (int)expected);
	if (length != (int)expected)
		return fail(error, error_size, "invalid %s payload or decoded length", codec);
	return 0;
}

static int decode_zstd(const uint8_t *input, uint32_t input_length, uint8_t *output,
		       uint32_t expected, char *error, size_t error_size)
{
	typedef size_t (*frame_fn)(const void *, size_t);
	typedef size_t (*decode_fn)(void *, size_t, const void *, size_t);
	typedef unsigned (*error_fn)(size_t);
	static void *handle;
	static frame_fn frame_size;
	static decode_fn decode;
	static error_fn is_error;
	size_t frame, length;
	if (!decode) {
		if (!handle)
			handle = load_decoder_library("zstd", "libzstd.so.1", error, error_size);
		if (!handle ||
		    load_symbol(handle, "zstd", "ZSTD_findFrameCompressedSize",
				(void **)&frame_size, error, error_size) ||
		    load_symbol(handle, "zstd", "ZSTD_decompress",
				(void **)&decode, error, error_size) ||
		    load_symbol(handle, "zstd", "ZSTD_isError",
				(void **)&is_error, error, error_size))
			return -1;
	}
	frame = frame_size(input, input_length);
	if (is_error(frame) || frame != input_length)
		return fail(error, error_size, "invalid zstd frame extent");
	length = decode(output, expected, input, input_length);
	if (is_error(length) || length != expected)
		return fail(error, error_size, "invalid zstd payload or decoded length");
	return 0;
}

static int decode_deflate(const uint8_t *input, uint32_t input_length, uint8_t *output,
			  uint32_t expected, char *error, size_t error_size)
{
	typedef int (*init_fn)(z_streamp, int, const char *, int);
	typedef int (*inflate_fn)(z_streamp, int);
	typedef int (*end_fn)(z_streamp);
	typedef const char *(*version_fn)(void);
	static void *handle;
	static init_fn init;
	static inflate_fn inflate_run;
	static end_fn end;
	static version_fn version;
	z_stream stream = { 0 };
	int status, end_status;
	if (!inflate_run) {
		if (!handle)
			handle = load_decoder_library("deflate", "libz.so.1", error, error_size);
		if (!handle ||
		    load_symbol(handle, "deflate", "inflateInit2_", (void **)&init, error, error_size) ||
		    load_symbol(handle, "deflate", "inflate", (void **)&inflate_run, error, error_size) ||
		    load_symbol(handle, "deflate", "inflateEnd", (void **)&end, error, error_size) ||
		    load_symbol(handle, "deflate", "zlibVersion", (void **)&version, error, error_size))
			return -1;
	}
	stream.next_in = (Bytef *)input;
	stream.avail_in = input_length;
	stream.next_out = output;
	stream.avail_out = expected;
	if (init(&stream, -11, version(), sizeof(stream)) != Z_OK)
		return fail(error, error_size, "cannot initialize raw deflate decoder");
	status = inflate_run(&stream, Z_FINISH);
	end_status = end(&stream);
	if (end_status != Z_OK || status != Z_STREAM_END || stream.total_out != expected ||
	    stream.avail_in != 0 || stream.avail_out != 0)
		return fail(error, error_size, "invalid raw deflate stream or decoded length");
	return 0;
}
#endif

static int decode_payload(const char *algorithm, const uint8_t *input, uint32_t input_length,
			  uint8_t *output, uint32_t expected,
			  char *error, size_t error_size)
{
	if (!input_length || input_length >= expected ||
	    (expected != DB_BLOCK && expected != DB_REGION))
		return fail(error, error_size, "invalid compressed buffer bounds");
#ifndef DYNBLK_NO_DYNAMIC_CODECS
	if (!strcmp(algorithm, "lz4") || !strcmp(algorithm, "lz4hc"))
		return decode_lz4(algorithm, input, input_length, output, expected, error, error_size);
	if (!strcmp(algorithm, "zstd"))
		return decode_zstd(input, input_length, output, expected, error, error_size);
#else
	if (!strcmp(algorithm, "lz4") || !strcmp(algorithm, "lz4hc") ||
	    !strcmp(algorithm, "zstd") || !strcmp(algorithm, "deflate"))
		return fail(error, error_size,
			"cannot validate %s: optional decoder is not included in the initrd build",
			algorithm);
#endif
	if (!strcmp(algorithm, "lzo") || !strcmp(algorithm, "lzo-rle")) {
		size_t length = expected;
		if (dynblk_lzo1x_decompress_safe(input, input_length, output, &length) ||
		    length != expected)
			return fail(error, error_size, "invalid %s payload or decoded length", algorithm);
		return 0;
	}
#ifndef DYNBLK_NO_DYNAMIC_CODECS
	if (!strcmp(algorithm, "deflate"))
		return decode_deflate(input, input_length, output, expected, error, error_size);
#endif
	if (!strcmp(algorithm, "842"))
		return fail(error, error_size,
			"cannot validate 842: no qualified userspace decoder");
	return fail(error, error_size, "cannot validate %s: decoder unavailable", algorithm);
}


static void *dh_alloc(void *ctx, size_t size) { (void)ctx; return calloc(1, size); }
static void dh_free(void *ctx, void *p) { (void)ctx; free(p); }
static int dh_reserve_files(struct de_host *h, unsigned int need)
{
    int *p;
    unsigned int i, slots = h->file_slots ? h->file_slots : 8;
    if (need > DE_MAX_FILES) return -EFBIG;
    if (need <= h->file_slots) return 0;
    while (slots < need) slots *= 2;
    if (slots > DE_MAX_FILES) slots = DE_MAX_FILES;
    p = realloc(h->files, (size_t)slots * sizeof(*p));
    if (!p) return -ENOMEM;
    for (i = h->file_slots; i < slots; i++) p[i] = -1;
    h->files = p; h->file_slots = slots;
    return 0;
}
static int dh_open(void *opaque, unsigned int id, const char *name, bool create)
{
    struct de_host *h = opaque;
    struct stat st, other;
    unsigned int i;
    int fd, ret, flags = h->readonly ? O_RDONLY : O_RDWR;
    if (id >= DE_MAX_FILES || (create && h->readonly)) return -EINVAL;
    ret = dh_reserve_files(h, id + 1);
    if (ret) return ret;
    if (h->files[id] >= 0) return -EINVAL;
    if (strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) return -EINVAL;
    flags |= O_NOFOLLOW | O_CLOEXEC | O_NOATIME | O_NONBLOCK;
    if (h->direct) flags |= O_DIRECT;
    if (create) flags |= O_CREAT | O_EXCL;
    fd = openat(h->directory, name, flags, 0600);
    if (fd < 0) return -errno;
    h->files[id] = fd;
    if (fstat(fd, &st)) return -errno;
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_size > (off_t)DE_PART_LIMIT ||
        (h->strict && (st.st_uid || (st.st_mode & 0022)))) return -EPERM;
    for (i = 0; i < h->file_slots; i++) if (i != id && h->files[i] >= 0) {
        if (fstat(h->files[i], &other)) return -errno;
        if (other.st_dev == st.st_dev && other.st_ino == st.st_ino) return -EUCLEAN;
    }
    return flock(fd, (h->readonly ? LOCK_SH : LOCK_EX) | LOCK_NB) ? -errno : 0;
}
static int dh_size(void *opaque, unsigned int id, db_u64 *size)
{
    struct de_host *h = opaque;
    struct stat st;
    if (id >= h->file_slots || h->files[id] < 0) return -EBADF;
    if (fstat(h->files[id], &st)) return -errno;
    *size = st.st_size; return 0;
}
static int dh_resize(void *opaque, unsigned int id, db_u64 size)
{
    struct de_host *h = opaque;
    if (h->readonly) return -EROFS;
    if (id >= h->file_slots || h->files[id] < 0 || size > DE_PART_LIMIT) return -EINVAL;
    return ftruncate(h->files[id], (off_t)size) ? -errno : 0;
}
static int dh_io(void *opaque, unsigned int id, void *buffer, size_t bytes, db_u64 offset, bool write)
{
    struct de_host *h = opaque;
    db_u8 *buf = buffer;
    db_u64 oldsize, target;
    int fd, ret;
    if (id >= h->file_slots || h->files[id] < 0 || offset > DE_PART_LIMIT || bytes > DE_PART_LIMIT - offset)
        return -EINVAL;
    if (write && h->readonly) return -EROFS;
    fd = h->files[id];
    if (write) h->write_bytes += bytes; else h->read_bytes += bytes;
    if (!h->direct) {
        while (bytes) {
            ssize_t n = write ? pwrite(fd, buf, bytes, offset) : pread(fd, buf, bytes, offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return n < 0 ? -errno : -EIO;
            offset += n; buf += n; bytes -= n;
        }
        return 0;
    }
    ret = dh_size(h, id, &oldsize);
    if (ret) return ret;
    target = oldsize > offset + bytes ? oldsize : offset + bytes;
    while (bytes) {
        db_u64 base = offset / DE_PAGE * DE_PAGE;
        size_t within = offset % DE_PAGE, take = DE_GRAIN - within;
        size_t span;
        ssize_t got;
        if (take > bytes) take = bytes;
        span = (within + take + DE_PAGE - 1) / DE_PAGE * DE_PAGE;
        memset(h->bounce, 0, span);
        if (!write || within || take != span) {
            got = pread(fd, h->bounce, span, base);
            if (got < 0) return -errno;
            if (!write && (size_t)got < within + take) return -EIO;
            if (write && (db_u64)got < (oldsize > base ? (oldsize - base < span ? oldsize - base : span) : 0)) return -EIO;
        }
        if (write) {
            memcpy((db_u8 *)h->bounce + within, buf, take);
            got = pwrite(fd, h->bounce, span, base);
            if (got < 0) return -errno;
            if ((size_t)got != span) return -EIO;
        } else memcpy(buf, (db_u8 *)h->bounce + within, take);
        bytes -= take; buf += take; offset += take;
    }
    /* Aligned RMW must not change descriptor length on an eight-byte CID update. */
    if (write) {
        db_u64 actual;
        ret = dh_size(h, id, &actual);
        if (!ret && actual > target) ret = dh_resize(h, id, target);
    }
    return ret;
}
static int dh_sync(void *opaque, unsigned int id)
{
    struct de_host *h = opaque;
    if (h->readonly) return 0;
    if (id >= h->file_slots || h->files[id] < 0) return -EBADF;
    h->sync_count++;
    return fdatasync(h->files[id]) ? -errno : 0;
}
static int dh_sync_dir(void *opaque)
{
    struct de_host *h = opaque;
    return h->readonly ? 0 : fsync(h->directory) ? -errno : 0;
}
static int dh_punch(void *ctx, unsigned int id, db_u64 off, db_u64 bytes)
{
    struct de_host *h = ctx;
    if (h->readonly) return -EROFS;
    if (id >= h->file_slots || h->files[id] < 0 || off % DE_PAGE || bytes % DE_PAGE ||
        off > DE_PART_LIMIT || bytes > DE_PART_LIMIT - off) return -EINVAL;
    return fallocate(h->files[id], FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                     (off_t)off, (off_t)bytes) ? -errno : 0;
}
static int dh_codec(void *opaque, const char *name, bool encode, const void *src,
                    unsigned int length, void *dst, unsigned int *outlen)
{
    struct de_host *h = opaque;
    if (!encode) {
        int ret = decode_payload(name, src, length, dst, *outlen, h->error, sizeof(h->error));
        return ret ? -EUCLEAN : 0;
    }
#if !defined(DYNBLK_NO_DYNAMIC_CODECS) && defined(DYNBLK_ENGINE_TEST)
    /* Encoders are test-only: installed userspace never writes image payloads. */
    if (!strcmp(name, "lz4") || !strcmp(name, "lz4hc")) {
        static void *lib;
        int n;
        if (!lib) lib = dlopen("liblz4.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!lib) return -EOPNOTSUPP;
        if (!strcmp(name, "lz4hc")) {
            int (*fn)(const char *, char *, int, int, int) = dlsym(lib, "LZ4_compress_HC");
            if (!fn) return -EOPNOTSUPP;
            n = fn(src, dst, length, *outlen, 9);
        } else {
            int (*fn)(const char *, char *, int, int) = dlsym(lib, "LZ4_compress_default");
            if (!fn) return -EOPNOTSUPP;
            n = fn(src, dst, length, *outlen);
        }
        if (n <= 0) return -EIO;
        *outlen = n; return 0;
    }
    if (!strcmp(name, "zstd")) {
        static void *lib;
        size_t (*fn)(void *, size_t, const void *, size_t, int);
        size_t n;
        if (!lib) lib = dlopen("libzstd.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!lib) return -EOPNOTSUPP;
        fn = dlsym(lib, "ZSTD_compress");
        if (!fn) return -EOPNOTSUPP;
        n = fn(dst, *outlen, src, length, 3);
        if (!n || n > *outlen) return -EIO;
        *outlen = n; return 0;
    }
#endif
    return -EOPNOTSUPP;
}
int dh_init(struct de_host *h, struct de_io *io, const char *path,
            bool readonly, bool direct, bool strict)
{
    const char *slash = strrchr(path, '/');
    size_t n;
    memset(h, 0, sizeof(*h)); h->directory = -1;
    h->readonly = readonly; h->direct = direct; h->strict = strict;
    if (!slash || path[0] != '/' || !slash[1]) return -EINVAL;
    n = slash - path;
    if (!n) n = 1;
    if (n >= sizeof(h->parent)) return -ENAMETOOLONG;
    memcpy(h->parent, path, n); h->parent[n] = 0;
    if (strict && dynblk_safe_path(path, !readonly, h->error, sizeof(h->error))) return -EPERM;
    h->directory = open(h->parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (h->directory < 0) return -errno;
    if (direct && posix_memalign(&h->bounce, DE_PAGE, DE_GRAIN)) return -ENOMEM;
    *io = (struct de_io){.ctx = h, .alloc = dh_alloc, .free = dh_free, .open = dh_open,
        .io = dh_io, .size = dh_size, .resize = dh_resize, .sync = dh_sync,
        .sync_dir = dh_sync_dir, .punch = dh_punch, .codec = dh_codec};
    return 0;
}
void dh_close(struct de_host *h)
{
    unsigned int i;
    for (i = 0; i < h->file_slots; i++) if (h->files[i] >= 0) close(h->files[i]);
    if (h->directory >= 0) close(h->directory);
    free(h->files); free(h->bounce);
}
