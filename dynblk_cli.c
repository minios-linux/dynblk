// SPDX-License-Identifier: GPL-2.0-or-later
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "dynblk_check.h"
#include "dynblk_common.h"
#include "dynblk_uapi.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CONTROL "/dev/dynblk-control"
#define DEVICE_PREFIX "/dev/dynblk"
#define DEFAULT_SIZE (16ULL << 30)
#define DEFAULT_PART_SIZE (4000ULL << 20)
#define MIN_PART_SIZE (1ULL << 20)

struct held_attachment {
	int fd, control;
	uint32_t device;
	uint64_t cookie;
	bool loaded_before, autoclear;
};

static int error_message(const char *message)
{
	fprintf(stderr, "dynblk: %s\n", message);
	return 1;
}
static int system_error(const char *context)
{
	char message[512];
	snprintf(message, sizeof(message), "%s: %s", context, strerror(errno));
	return error_message(message);
}

static void usage(FILE *stream)
{
	fputs("Usage:\n"
	      "  dynblk create PATH [--format dynblk|vmdk] [--cache MODE] [--size SIZE] [--compression CODEC] [--part-size SIZE] [--map-memory-mb N] [--module FILE] [--execute]\n"
	      "  dynblk load PATH [--format auto|dynblk|vmdk] [--cache MODE] [--read-only] [--map-memory-mb N] [--module FILE] [--execute]\n"
	      "  dynblk limits [--format dynblk|vmdk] [--part-size SIZE] [--json]\n"
	      "  dynblk status /dev/dynblkN [--json]\n"
	      "  dynblk grow /dev/dynblkN SIZE [--execute]\n"
              "  dynblk reclaim /dev/dynblkN [--compact] [--scan-zeroes] [--max-steps N] [--execute] [--json]\n"
	      "  dynblk unload /dev/dynblkN [--execute]\n"
	      "  dynblk inspect PATH --metadata-only [--json]\n"
	      "  dynblk check PATH [--json]\n"
	      "\n--map-memory-mb sets a bounded metadata cache (default 1 MiB, range 1..64).\nCache modes: writeback, writethrough, none, directsync, unsafe.\n"
	      "Device management only: no mkfs, mount, fsck or filesystem resize.\n",
	      stream);
}

static bool help_arg(const char *arg)
{
	return !strcmp(arg, "-h") || !strcmp(arg, "--help");
}

static int parse_error(const char *message)
{
	if (message)
		fprintf(stderr, "dynblk: %s\n", message);
	usage(stderr);
	return 2;
}
static void json_string(const char *text)
{
	const unsigned char *p = (const unsigned char *)text;
	putchar('"');
	for (; *p; p++) {
		switch (*p) {
		case '"': fputs("\\\"", stdout); break;
		case '\\': fputs("\\\\", stdout); break;
		case '\b': fputs("\\b", stdout); break;
		case '\f': fputs("\\f", stdout); break;
		case '\n': fputs("\\n", stdout); break;
		case '\r': fputs("\\r", stdout); break;
		case '\t': fputs("\\t", stdout); break;
		default:
			if (*p < 0x20)
				printf("\\u%04x", *p);
			else
				putchar(*p);
		}
	}
	putchar('"');
}

static bool device_index(const char *path, unsigned int *index)
{
	const char *p;
	char *end;
	unsigned long value;

	if (!path || strncmp(path, DEVICE_PREFIX, strlen(DEVICE_PREFIX)))
		return false;
	p = path + strlen(DEVICE_PREFIX);
	if (!*p)
		return false;
	errno = 0;
	value = strtoul(p, &end, 10);
	if (errno || *end || value >= DYNBLK_MAX_DEVICES)
		return false;
	if (index)
		*index = (unsigned int)value;
	return true;
}

static bool module_loaded(void)
{
	struct stat st;
	return !lstat("/sys/module/dynblk", &st);
}
static int run_program_internal(char *const argv[], bool quiet)
{
	pid_t pid;
	int status;
	pid = fork();
	if (pid < 0)
		return system_error("fork");
	if (!pid) {
		if (quiet) {
			int nullfd = open("/dev/null", O_WRONLY | O_CLOEXEC);
			if (nullfd >= 0) {
				dup2(nullfd, STDERR_FILENO);
				close(nullfd);
			}
		}
		clearenv();
		setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
		setenv("LC_ALL", "C", 1);
		execvp(argv[0], argv);
		if (!quiet)
			perror(argv[0]);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) < 0)
		return system_error("waitpid");
	if (!WIFEXITED(status) || WEXITSTATUS(status)) {
		char message[256];
		if (quiet)
			return 1;
		snprintf(message, sizeof(message), "%s failed%s%s", argv[0],
			 WIFEXITED(status) ? " with exit " : "",
			 WIFEXITED(status) ? "status" : "");
		return error_message(message);
	}
	return 0;
}

static int run_program(char *const argv[])
{
	return run_program_internal(argv, false);
}

static int read_status(const char *device, struct dynblk_status *status,
		       bool writable, int *fd_out)
{
	struct stat st;
	char context[320];
	int fd = open(device, (writable ? O_RDWR : O_RDONLY) |
			     O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		snprintf(context, sizeof(context), "open %s", device);
		return system_error(context);
	}
	if (fstat(fd, &st) || !S_ISBLK(st.st_mode)) {
		close(fd);
		return error_message("status requires a block device");
	}
	if (ioctl(fd, DYNBLK_GET, status)) {
		close(fd);
		return system_error("DYNBLK_GET");
	}
	if (fd_out)
		*fd_out = fd;
	else
		close(fd);
	return 0;
}

static int validate_status(struct dynblk_status *status, char codec[DYNBLK_CODEC_MAX])
{
	char error[256];
	_Static_assert(sizeof(struct dynblk_status) == 176, "dynblk status ABI");
	if (status->version != DYNBLK_ABI_VERSION || status->flags & ~(DYNBLK_STATUS_FENCED | DYNBLK_STATUS_READ_ONLY | DYNBLK_STATUS_AUTOCLEAR) ||
	    (status->storage_format != DE_NATIVE && status->storage_format != DE_VMDK) ||
        status->block_size != (status->storage_format == DE_VMDK ? 512U : DE_PAGE) ||
        status->cache_mode > DE_UNSAFE || status->max_parts != DYNBLK_MAX_PARTS ||
	    status->tree_height != DYNBLK_TREE_HEIGHT ||
	    status->compression_region_bytes != DYNBLK_COMPRESSION_REGION ||
	    status->max_capacity != de_capacity_limit(status->storage_format, status->part_limit) ||
	    dynblk_codec_field(status->algorithm, codec, error, sizeof(error)))
		return error_message("unsupported dynblk status ABI");
	return 0;
}

static int ensure_module(const char *module)
{
	char *modprobe[] = { "modprobe", "dynblk", NULL };
	char *insmod[] = { "insmod", (char *)module, NULL };
	int ret;

	if (module_loaded())
		return 0;
	ret = run_program(module ? insmod : modprobe);
	if (ret)
		return ret;
	if (!module_loaded())
		return error_message("module loader returned success but dynblk is not loaded");
	return 0;
}

static void unload_module_if_new(bool loaded_before)
{
	char *command[] = { "rmmod", "dynblk", NULL };

	if (!loaded_before)
		run_program_internal(command, true);
}

static int open_control(void)
{
	struct stat st;
	unsigned int attempt;
	int fd = -1;

	for (attempt = 0; attempt < 20; attempt++) {
		fd = open(CONTROL, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
		if (fd >= 0)
			break;
		if (errno != ENOENT && errno != ENODEV)
			break;
		usleep(50000);
	}
	if (fd < 0) {
		system_error("open /dev/dynblk-control");
		return -1;
	}
	if (fstat(fd, &st) || !S_ISCHR(st.st_mode)) {
		close(fd);
		error_message("/dev/dynblk-control is not a character device");
		return -1;
	}
	return fd;
}

static void print_status(const char *device, const struct dynblk_status *status,
			 const char *codec, bool json)
{
	char uuid[37];
	dynblk_uuid_string(status->uuid, uuid);
	if (json) {
		printf("{\"abi_version\":%u,\"active_parts\":%u,\"autoclear\":%s,\"block_size\":%u,"
		       "\"capacity_bytes\":%" PRIu64 ",\"compression\":", status->version,
		       status->active_parts, status->flags & DYNBLK_STATUS_AUTOCLEAR ? "true" : "false",
		       status->block_size, (uint64_t)status->capacity);
		json_string(codec);
		printf(",\"compression_region_bytes\":%" PRIu64 ",\"device\":\"%s\","
		       "\"fenced\":%s,\"flags\":%u,\"generation\":%" PRIu64,
		       (uint64_t)status->compression_region_bytes, device,
		       status->flags & 1 ? "true" : "false", status->flags,
		       (uint64_t)status->generation);
		printf(",\"map_memory_bytes\":%" PRIu64 ",\"max_capacity_bytes\":%" PRIu64
		       ",\"max_parts\":%u,\"original_bytes\":%" PRIu64,
		       (uint64_t)status->map_memory_bytes, (uint64_t)status->max_capacity,
		       status->max_parts, (uint64_t)status->original_bytes);
		printf(",\"part_limit_bytes\":%" PRIu64 ",\"physical_bytes\":%" PRIu64
		       ",\"read_only\":%s,\"stored_bytes\":%" PRIu64 ",\"tree_height\":%u,\"uuid\":",
		       (uint64_t)status->part_limit, (uint64_t)status->physical_bytes,
		       status->flags & DYNBLK_STATUS_READ_ONLY ? "true" : "false",
		       (uint64_t)status->stored_bytes, status->tree_height);
		json_string(uuid);
        printf(",\"storage_format\":"); json_string(de_format_name(status->storage_format));
        printf(",\"cache\":"); json_string(de_cache_name(status->cache_mode));
        printf(",\"engine_memory_bytes\":%" PRIu64 ",\"cache_hits\":%" PRIu64
               ",\"cache_misses\":%" PRIu64 ",\"flush_count\":%" PRIu64
               ",\"capabilities\":%u}\n",
               (uint64_t)status->engine_memory_bytes, (uint64_t)status->cache_hits,
               (uint64_t)status->cache_misses, (uint64_t)status->flush_count, status->capabilities);
		return;
	}
	printf("abi_version: %u\nflags: %u\nuuid: %s\ncompression: %s\n"
	       "capacity_bytes: %" PRIu64 "\noriginal_bytes: %" PRIu64 "\n"
	       "stored_bytes: %" PRIu64 "\nphysical_bytes: %" PRIu64 "\n"
	       "generation: %" PRIu64 "\npart_limit_bytes: %" PRIu64 "\n"
	       "active_parts: %u\nblock_size: %u\nmax_parts: %u\ntree_height: %u\n"
	       "map_memory_bytes: %" PRIu64 "\nmax_capacity_bytes: %" PRIu64 "\n"
	       "compression_region_bytes: %" PRIu64 "\nfenced: %s\ndevice: %s\n",
	       status->version, status->flags, uuid, codec, (uint64_t)status->capacity,
	       (uint64_t)status->original_bytes, (uint64_t)status->stored_bytes,
	       (uint64_t)status->physical_bytes, (uint64_t)status->generation,
	       (uint64_t)status->part_limit, status->active_parts, status->block_size,
	       status->max_parts, status->tree_height, (uint64_t)status->map_memory_bytes,
	       (uint64_t)status->max_capacity, (uint64_t)status->compression_region_bytes,
	       status->flags & 1 ? "true" : "false", device);
	printf("read_only: %s\nautoclear: %s\n",
	       status->flags & DYNBLK_STATUS_READ_ONLY ? "true" : "false",
	       status->flags & DYNBLK_STATUS_AUTOCLEAR ? "true" : "false");
    printf("storage_format: %s\ncache: %s\nengine_memory_bytes: %" PRIu64
           "\ncache_hits: %" PRIu64 "\ncache_misses: %" PRIu64 "\nflush_count: %" PRIu64
           "\ncapabilities: %u\n", de_format_name(status->storage_format),
           de_cache_name(status->cache_mode), (uint64_t)status->engine_memory_bytes,
           (uint64_t)status->cache_hits, (uint64_t)status->cache_misses,
           (uint64_t)status->flush_count, status->capabilities);

}

static int command_status(int argc, char **argv)
{
	struct dynblk_status status;
	char codec[DYNBLK_CODEC_MAX];
	bool json = false;

	if (argc == 2 && !strcmp(argv[1], "--json"))
		json = true;
	else if (argc != 1)
		return parse_error("invalid status arguments");
	if (!device_index(argv[0], NULL))
		return parse_error("status requires /dev/dynblkN");
	if (read_status(argv[0], &status, false, NULL) || validate_status(&status, codec))
		return 1;
	print_status(argv[0], &status, codec, json);
	return 0;
}
static void print_inspection(const struct dynblk_inspection *r, bool json)
{
    char uuid[37];
    dynblk_uuid_string(r->uuid, uuid);
    if (json) {
        printf("{\"format\":1,\"storage_format\":");
        json_string(de_format_name(r->storage_format));
        printf(",\"path\":"); json_string(r->path);
        printf(",\"uuid\":"); json_string(uuid);
        printf(",\"compression\":"); json_string(r->compression);
        printf(",\"capacity_bytes\":%" PRIu64 ",\"part_limit_bytes\":%" PRIu64
               ",\"physical_bytes\":%" PRIu64 ",\"primary_physical_bytes\":%" PRIu64
               ",\"generation\":%" PRIu64 ",\"mapped_grains\":%" PRIu64
               ",\"stored_bytes\":%" PRIu64 ",\"engine_memory_bytes\":%" PRIu64
               ",\"active_parts\":%u,\"fully_validated\":%s,\"validation_scope\":",
               r->capacity, r->part_limit, r->physical_bytes, r->primary_bytes,
               r->generation, r->mapped_grains, r->stored_bytes, r->memory_bytes,
               r->active_parts, r->fully_validated ? "true" : "false");
        json_string(r->fully_validated ? "mapping-and-payload-readability" : "mapping-metadata");
        printf(",\"checksum_scope\":\"native-compressed-payload-only\"}\n");
    } else {
        printf("path: %s\nformat: 1\nstorage_format: %s\nuuid: %s\ncompression: %s\n"
               "capacity_bytes: %" PRIu64 "\nphysical_bytes: %" PRIu64
               "\nmapped_grains: %" PRIu64 "\nstored_bytes: %" PRIu64
               "\nengine_memory_bytes: %" PRIu64 "\nactive_parts: %u\nfully_validated: %s\n"
               "checksum_scope: native-compressed-payload-only\n",
               r->path, de_format_name(r->storage_format), uuid, r->compression,
               r->capacity, r->physical_bytes, r->mapped_grains, r->stored_bytes,
               r->memory_bytes, r->active_parts, r->fully_validated ? "true" : "false");
    }
}

static int command_inspect(int argc, char **argv, bool full)
{
	struct dynblk_inspection result;
	char error[512];
	bool json = false, metadata = false;
	int i;
	if (argc < 1)
		return parse_error("missing volume path");
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--json")) json = true;
		else if (!strcmp(argv[i], "--metadata-only")) metadata = true;
		else return parse_error("invalid inspect/check option");
	}
	if ((!full && !metadata) || (full && metadata))
		return parse_error(full ? "check does not accept --metadata-only" :
				   "inspect requires --metadata-only");
	if (dynblk_inspect_volume(argv[0], full, &result, error, sizeof(error)))
		return error_message(error);
	print_inspection(&result, json);
	return 0;
}
static int validate_volume_path(const char *path, bool create)
{
    char error[512];
    struct stat st;
    const char *base = strrchr(path, '/');
    const unsigned char *p;
    if (!base || !base[1] || strlen(base + 1) > DE_NAME - 16 || base[1] == '.' || base[1] == '-')
        return error_message("invalid volume filename");
    for (p = (const unsigned char *)(base + 1); *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.'))
            return error_message("volume filename must contain only letters, digits, dot, underscore and hyphen");
    if (dynblk_safe_path(path, create, error, sizeof(error))) return error_message(error);
    if (create && (!lstat(path, &st) || errno != ENOENT))
        return error_message("create requires an unused primary filename; existing storage preserved");
    return 0;
}
static int parse_format(const char *text)
{
    if (!strcmp(text, "auto")) return DE_AUTO;
    if (!strcmp(text, "dynblk")) return DE_NATIVE;
    if (!strcmp(text, "vmdk")) return DE_VMDK;
    return -1;
}

static int validate_module(const char *path)
{
	char error[512];
	const char *base;
	if (!path)
		return 0;
	base = strrchr(path, '/');
	if (!base || strcmp(base + 1, "dynblk.ko"))
		return error_message("--module must name an existing dynblk.ko");
	if (dynblk_safe_path(path, false, error, sizeof(error)))
		return error_message(error);
	return 0;
}

static int parse_map_memory(const char *text, uint32_t *value)
{
	char *end;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno || !*text || *end || !parsed || parsed > DYNBLK_MAX_MAP_MEMORY_MB)
		return parse_error("--map-memory-mb must be an integer from 1 to 64");
	*value = (uint32_t)parsed;
	return 0;
}

static int execute_attach(const char *path, const char *module, bool create,
			  uint64_t size, uint64_t part_size, const char *codec,
			  uint32_t map_memory_mb, bool read_only, struct held_attachment *held,
                          unsigned int format, unsigned int cache_mode)
{
	struct dynblk_attach request = {
		.version = DYNBLK_ABI_VERSION,
		.flags = (create ? DYNBLK_ATTACH_CREATE : 0) |
			 (read_only ? DYNBLK_ATTACH_READ_ONLY : 0),
		.device = -1,
		.map_memory_mb = map_memory_mb,
        .storage_format = format, .cache_mode = cache_mode,
	};
	struct dynblk_detach detach = { .version = DYNBLK_ABI_VERSION };
	struct dynblk_status status;
	char device[64], actual_codec[DYNBLK_CODEC_MAX];
	bool loaded_before = module_loaded();
	int fd, device_fd = -1, result = 1;
	uint64_t cookie;

	if (strlen(path) >= sizeof(request.path))
		return error_message("volume path is too long");
	memcpy(request.path, path, strlen(path) + 1);
	if (create) {
		request.size_bytes = size;
		request.part_limit_bytes = part_size;
		if (strlen(codec) >= sizeof(request.algorithm))
			return error_message("compression name is too long");
		memcpy(request.algorithm, codec, strlen(codec) + 1);
	}
	if (ensure_module(module))
		return 1;
	fd = open_control();
	if (fd < 0) {
		unload_module_if_new(loaded_before);
		return 1;
	}
	/* Autoclear removes the disk from sysfs before its private backing files
	 * finish closing. A mount immediately after umount can meet their flock.
	 * Retry only this transient lock conflict, and only for helper loads;
	 * never retry creation, corruption, admission or general I/O failures. */
	for (unsigned int attempt = 0;; attempt++) {
		result = ioctl(fd, DYNBLK_ATTACH, &request);
		if (!result || !held || create || errno != EAGAIN || attempt == 39)
			break;
		usleep(50000);
	}
	if (result) {
		int saved = errno;
		close(fd);
		unload_module_if_new(loaded_before);
		errno = saved;
		return system_error("DYNBLK_ATTACH");
	}
	result = 1; /* All post-attach validation failures must return failure. */
	snprintf(device, sizeof(device), DEVICE_PREFIX "%d", request.device);
	if (read_status(device, &status, false, &device_fd) || validate_status(&status, actual_codec))
		goto rollback;
	if (ioctl(device_fd, DYNBLK_COOKIE, &cookie) || cookie != request.cookie) {
		error_message("attachment identity changed before device open");
		goto rollback;
	}
	if (!!(status.flags & DYNBLK_STATUS_READ_ONLY) != read_only ||
        status.cache_mode != cache_mode || (format && status.storage_format != format)) {
		error_message("attached device has an unexpected read-only mode");
		goto rollback;
	}
	if (create && (status.capacity != size || status.part_limit != part_size ||
		       strcmp(actual_codec, codec))) {
		error_message("attached device does not match requested create parameters");
		goto rollback;
	}
	if (held) {
		held->fd = device_fd;
		held->control = fd;
		held->device = (uint32_t)request.device;
		held->cookie = request.cookie;
		held->loaded_before = loaded_before;
	} else {
		close(device_fd);
		close(fd);
		puts(device);
	}
	return 0;

rollback:
	if (device_fd >= 0)
		close(device_fd);
	detach.device = (uint32_t)request.device;
	detach.cookie = request.cookie;
	if (ioctl(fd, DYNBLK_DETACH, &detach))
		system_error("rollback DYNBLK_DETACH");
	close(fd);
	unload_module_if_new(loaded_before);
	return result;
}

static int command_attach(int argc, char **argv, bool create)
{
	const char *path, *module = NULL, *codec = "none";
	uint64_t size = DEFAULT_SIZE, part_size = DEFAULT_PART_SIZE;
	uint32_t map_memory_mb = DYNBLK_DEFAULT_MAP_MEMORY_MB;
	bool execute = false, read_only = false;
    unsigned int format = create ? DE_NATIVE : DE_AUTO, cache_mode = DE_WRITEBACK;
    const char *subformat = NULL;
	char error[256];
	int i;

	if (argc < 1)
		return parse_error("missing volume path");
	path = argv[0];
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--execute")) execute = true;
		else if (!create && !strcmp(argv[i], "--read-only")) read_only = true;
        else if (!strcmp(argv[i], "--format") && i + 1 < argc) {
            int parsed = parse_format(argv[++i]);
            if (parsed < 0 || (create && parsed == DE_AUTO)) return parse_error("unsupported format");
            format = (unsigned int)parsed;
        } else if (!strcmp(argv[i], "--cache") && i + 1 < argc) {
            int parsed = de_cache_parse(argv[++i]);
            if (parsed < 0) return parse_error("unsupported cache policy");
            cache_mode = (unsigned int)parsed;
        } else if (create && !strcmp(argv[i], "--subformat") && i + 1 < argc) subformat = argv[++i];
		else if (!strcmp(argv[i], "--module") && i + 1 < argc) module = argv[++i];
		else if (!strcmp(argv[i], "--map-memory-mb") && i + 1 < argc) {
			if (parse_map_memory(argv[++i], &map_memory_mb)) return 2;
		} else if (create && !strcmp(argv[i], "--size") && i + 1 < argc) {
			if (dynblk_parse_size(argv[++i], &size, error, sizeof(error))) return parse_error(error);
		} else if (create && !strcmp(argv[i], "--part-size") && i + 1 < argc) {
			if (dynblk_parse_size(argv[++i], &part_size, error, sizeof(error))) return parse_error(error);
		} else if (create && !strcmp(argv[i], "--compression") && i + 1 < argc) codec = argv[++i];
		else return parse_error(create ? "invalid create option" : "invalid load option");
	}
	if (subformat && (format != DE_VMDK || strcmp(subformat, "twoGbMaxExtentSparse")))
        return parse_error("VMDK supports only twoGbMaxExtentSparse");
    if (create && format == DE_VMDK && (strcmp(codec, "none") || part_size != DEFAULT_PART_SIZE))
        return parse_error("split VMDK requires compression=none and standard extent geometry");
    if (validate_volume_path(path, create) || validate_module(module))
		return 1;
	if (create && !dynblk_codec_name(codec))
		return parse_error("unsupported create codec");
	if (create && (dynblk_aligned_size(size, DB_BLOCK, de_capacity_limit(format, part_size), error, sizeof(error)) ||
			 dynblk_aligned_size(part_size, MIN_PART_SIZE, DEFAULT_PART_SIZE, error, sizeof(error))))
		return parse_error(error);
	if (execute && geteuid() != 0)
		return error_message("mutations require root and --execute");
	if (!execute) {
        printf("format=%s cache=%s\n", de_format_name(format), de_cache_name(cache_mode));
        char map_budget[32];

		if (map_memory_mb)
			snprintf(map_budget, sizeof(map_budget), "%u", map_memory_mb);
		else
			strcpy(map_budget, "auto");
		if (create)
			printf("DRY RUN: ensure dynblk module; DYNBLK_ATTACH path=\"%s\" create=1 size_bytes=%" PRIu64
			       " algorithm=%s part_limit_bytes=%" PRIu64 " map_memory_mb=%s; next free /dev/dynblkN\n",
			       path, size, codec, part_size, map_budget);
		else
			printf("DRY RUN: ensure dynblk module; DYNBLK_ATTACH path=\"%s\" map_memory_mb=%s%s; next free /dev/dynblkN\n",
			       path, map_budget, read_only ? " read_only=1" : "");
		return 0;
	}
	return execute_attach(path, module, create, size, part_size, codec, map_memory_mb, read_only, NULL, format, cache_mode);
}

static int command_grow(int argc, char **argv)
{
	struct dynblk_status status;
	uint64_t size;
	bool execute = false;
	char error[256], codec[DYNBLK_CODEC_MAX];
	int fd;

	if (argc == 3 && !strcmp(argv[2], "--execute")) execute = true;
	else if (argc != 2) return parse_error("invalid grow arguments");
	if (!device_index(argv[0], NULL)) return parse_error("grow requires /dev/dynblkN");
	if (dynblk_parse_size(argv[1], &size, error, sizeof(error)) ||
	    dynblk_aligned_size(size, DB_BLOCK, DYNBLK_MAX_CAPACITY, error, sizeof(error)))
		return parse_error(error);
	if (!execute) {
		printf("DRY RUN: DYNBLK_GROW %s absolute capacity %" PRIu64
		       " bytes; no filesystem resize\n", argv[0], size);
		return 0;
	}
	if (geteuid() != 0) return error_message("mutations require root and --execute");
	fd = -1;
	if (read_status(argv[0], &status, true, &fd) || validate_status(&status, codec)) {
		if (fd >= 0) close(fd);
		return 1;
	}
	if (ioctl(fd, DYNBLK_GROW, &size)) {
		close(fd);
		return system_error("DYNBLK_GROW");
	}
	close(fd);
	return 0;
}

static int command_unload(int argc, char **argv)
{
	struct dynblk_detach request = { .version = DYNBLK_ABI_VERSION };
	struct dynblk_status status;
	char codec[DYNBLK_CODEC_MAX];
	uint64_t cookie = 0;
	bool execute = false;
	unsigned int index, attempt;
	int device_fd = -1, fd;

	if (argc == 2 && !strcmp(argv[1], "--execute")) execute = true;
	else if (argc != 1) return parse_error("invalid unload arguments");
	if (!device_index(argv[0], &index)) return parse_error("unload requires /dev/dynblkN");
	if (!execute) {
		printf("DRY RUN: DYNBLK_DETACH %s; no unmount or filesystem operations\n", argv[0]);
		return 0;
	}
	if (geteuid() != 0) return error_message("mutations require root and --execute");
	if (read_status(argv[0], &status, false, &device_fd) || validate_status(&status, codec)) {
		if (device_fd >= 0) close(device_fd);
		return 1;
	}
	if (ioctl(device_fd, DYNBLK_COOKIE, &cookie)) {
		int saved = errno;
		close(device_fd);
		errno = saved;
		return system_error("DYNBLK_COOKIE");
	}
	if (!cookie) {
		close(device_fd);
		return error_message("dynblk device returned an invalid attachment cookie");
	}
	close(device_fd);
	fd = open_control();
	if (fd < 0)
		return 1;
	request.device = index;
	request.cookie = cookie;
	for (attempt = 0; attempt < 10; attempt++) {
		if (!ioctl(fd, DYNBLK_DETACH, &request)) {
			close(fd);
			return 0;
		}
		if (errno != EBUSY)
			break;
		usleep(100000);
	}
	{
		int saved = errno;
		close(fd);
		errno = saved;
	}
	return system_error("DYNBLK_DETACH");
}

/* No userspace writes to the backing files. Hold the attachment fd/cookie
 * while the kernel advances bounded steps, yielding to ordinary I/O between. */
static int command_reclaim(int argc, char **argv)
{
    struct dynblk_reclaim request = { .version = DYNBLK_ABI_VERSION };
    struct dynblk_status status;
    uint64_t scanned = 0, moved = 0, unmapped = 0, punched = 0, truncated = 0;
    uint64_t steps = 0, max_steps = UINT64_MAX;
    char codec[DYNBLK_CODEC_MAX];
    bool execute = false, json = false;
    int i, fd = -1;
    if (!argc || !device_index(argv[0], NULL)) return parse_error("reclaim requires /dev/dynblkN");
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--execute")) execute = true;
        else if (!strcmp(argv[i], "--compact")) request.flags |= DYNBLK_RECLAIM_COMPACT;
        else if (!strcmp(argv[i], "--scan-zeroes")) request.flags |= DYNBLK_RECLAIM_ZEROES;
        else if (!strcmp(argv[i], "--json")) json = true;
        else if (!strcmp(argv[i], "--max-steps") && i + 1 < argc) {
            char *end;
            const char *text = argv[++i];
            errno = 0; max_steps = strtoull(text, &end, 10);
            if (errno || !*text || *text < '0' || *text > '9' || *end || !max_steps)
                return parse_error("--max-steps requires a positive integer");
        } else return parse_error("invalid reclaim option");
    }
    if (!execute) {
        if (json) printf("{\"dry_run\":true,\"compact\":%s,\"scan_zeroes\":%s}\n",
            request.flags & DYNBLK_RECLAIM_COMPACT ? "true" : "false",
            request.flags & DYNBLK_RECLAIM_ZEROES ? "true" : "false");
        else printf("DRY RUN: reclaim %s; %s; no filesystem trim or conversion\n", argv[0],
            request.flags & DYNBLK_RECLAIM_COMPACT ? "explicit live-data relocation" : "no live-data relocation");
        return 0;
    }
    if (geteuid() != 0) return error_message("mutations require root and --execute");
    if (read_status(argv[0], &status, true, &fd) || validate_status(&status, codec)) {
        if (fd >= 0) close(fd);
        return 1;
    }
    if (status.cache_mode == DE_UNSAFE || status.flags & DYNBLK_STATUS_READ_ONLY) {
        close(fd);
        return error_message("reclaim requires a writable attachment with flush barriers enabled (not unsafe)");
    }
    if (ioctl(fd, DYNBLK_COOKIE, &request.cookie)) goto ioctl_error;
    do {
        if (ioctl(fd, DYNBLK_RECLAIM, &request)) goto ioctl_error;
        steps++;
        scanned += request.scanned_bytes; moved += request.moved_bytes;
        unmapped += request.unmapped_bytes; punched += request.punch_bytes;
        truncated += request.truncated_bytes;
        if (!request.done && steps < max_steps) usleep(1000);
    } while (!request.done && steps < max_steps);
    close(fd);
    if (json)
        printf("{\"complete\":%s,\"steps\":%" PRIu64 ",\"scanned_bytes\":%" PRIu64
               ",\"moved_bytes\":%" PRIu64 ",\"unmapped_bytes\":%" PRIu64
               ",\"punch_bytes_requested\":%" PRIu64 ",\"truncated_bytes\":%" PRIu64 "}\n",
               request.done ? "true" : "false", steps, scanned, moved, unmapped, punched, truncated);
    else
        printf("complete: %s\nsteps: %" PRIu64 "\nscanned_bytes: %" PRIu64
               "\nmoved_bytes: %" PRIu64 "\nunmapped_bytes: %" PRIu64
               "\npunch_bytes_requested: %" PRIu64 "\ntruncated_bytes: %" PRIu64 "\n"
               "note: punched ranges and file-length reduction are not measured allocated-byte savings\n",
               request.done ? "true" : "false", steps, scanned, moved, unmapped, punched, truncated);
    return 0;
ioctl_error:
    {
        int saved = errno;
        close(fd); errno = saved;
        return system_error("DYNBLK_RECLAIM; completed steps retained, device not detached");
    }
}

#include "dynblk_mount.inc"

static int command_limits(int argc, char **argv)
{
    unsigned int format = DE_NATIVE;
    uint64_t part_size = DE_PART_LIMIT, limit;
    bool json = false;
    char error[256];
    int i;
    for (i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) json = true;
        else if (!strcmp(argv[i], "--format") && i + 1 < argc) {
            int parsed = parse_format(argv[++i]);
            if (parsed != DE_NATIVE && parsed != DE_VMDK) return parse_error("limits needs dynblk or vmdk format");
            format = parsed;
        } else if (!strcmp(argv[i], "--part-size") && i + 1 < argc) {
            if (dynblk_parse_size(argv[++i], &part_size, error, sizeof(error))) return parse_error(error);
        } else return parse_error("invalid limits option");
    }
    if (dynblk_aligned_size(part_size, DE_PAGE * 256, DE_PART_LIMIT, error, sizeof(error))) return parse_error(error);
    if (format == DE_VMDK && part_size != DE_PART_LIMIT) return parse_error("VMDK uses standard extent geometry");
    limit = de_capacity_limit(format, part_size);
    if (json)
        printf("{\"format\":1,\"storage_format\":\"%s\",\"max_capacity_bytes\":%" PRIu64
               ",\"max_capacity_mib\":%" PRIu64 ",\"max_parts\":%u,\"max_descriptor_bytes\":%u}\n",
               de_format_name(format), limit, limit >> 20, DE_MAX_PARTS, DE_DESCRIPTOR_MAX - 1);
    else
        printf("storage_format: %s\nformat: 1\nmax_capacity_bytes: %" PRIu64
               "\nmax_capacity_mib: %" PRIu64 "\nmax_parts: %u\nmax_descriptor_bytes: %u\n",
               de_format_name(format), limit, limit >> 20, DE_MAX_PARTS, DE_DESCRIPTOR_MAX - 1);
    return 0;
}

int main(int argc, char **argv)
{
	const char *command;
	const char *base = strrchr(argv[0], '/');
	base = base ? base + 1 : argv[0];
	if (!strcmp(base, "mount.dynblk"))
		return command_mount(argc - 1, argv + 1);
	if (argc == 2 && help_arg(argv[1])) {
		usage(stdout);
		return 0;
	}
	if (argc < 2)
		return parse_error("missing command");
	command = argv[1];
	if (argc >= 3 && help_arg(argv[2])) {
		usage(stdout);
		return 0;
	}
	if (!strcmp(command, "limits"))
        return command_limits(argc - 2, argv + 2);
	if (!strcmp(command, "create"))
		return command_attach(argc - 2, argv + 2, true);
	if (!strcmp(command, "load"))
		return command_attach(argc - 2, argv + 2, false);
	if (!strcmp(command, "status"))
		return command_status(argc - 2, argv + 2);
	if (!strcmp(command, "reclaim"))
		return command_reclaim(argc - 2, argv + 2);
	if (!strcmp(command, "grow"))
		return command_grow(argc - 2, argv + 2);
	if (!strcmp(command, "unload"))
		return command_unload(argc - 2, argv + 2);
	if (!strcmp(command, "inspect"))
		return command_inspect(argc - 2, argv + 2, false);
	if (!strcmp(command, "check"))
		return command_inspect(argc - 2, argv + 2, true);
	return parse_error("unknown command");
}
