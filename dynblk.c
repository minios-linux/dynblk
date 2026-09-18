// SPDX-License-Identifier: GPL-2.0-or-later
/* Self-contained format 1. See FORMAT.md before changing ordering. */
#include <linux/blk-mq.h>
#include <linux/capability.h>
#include <linux/crc32.h>
#include <linux/cred.h>
#include <linux/crypto.h>
#include <linux/fileattr.h>
#include <linux/filelock.h>
#include <linux/falloc.h>
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/magic.h>
#include <linux/mm.h>
#include <linux/miscdevice.h>
#include <linux/idr.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/random.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/uaccess.h>
#include <linux/uuid.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>
#include <linux/xattr.h>

#include "dynblk_uapi.h"

#include "dynblk_engine.c"
#define DB_BLOCK 4096U
#define DB_IO (256U * 1024)
#define DB_MIN_PART_LIMIT (1ULL << 20)
struct dynblk_device {
    int index;
    bool creating, detaching, read_only, autoclear, autoclear_pending, fenced;
    unsigned int map_memory_mb, openers, cache_mode;
    u64 cookie, part_limit;
    char *backing_filename;
    char codec_name[DYNBLK_CODEC_MAX];
    struct file *directory, **files;
    unsigned int file_slots;
    const struct cred *creation_cred;
    struct crypto_comp *codec;
    struct de_engine engine;
    struct mutex state_lock, lifecycle_lock;
    struct work_struct autoclear_work;
    struct delayed_work flush_work;
    void *raw, *direct_buffer;
    struct gendisk *disk;
    struct blk_mq_tag_set tags;
    struct workqueue_struct *worker;
};
static DEFINE_MUTEX(devices_lock);
static DEFINE_IDA(device_ids);
static struct dynblk_device *devices[DYNBLK_MAX_DEVICES];
static struct workqueue_struct *autoclear_queue;
static int detach_device(u32 index, u64 cookie, bool automatic);
static void autoclear_device(struct work_struct *work);
static int block_open(struct gendisk *disk, blk_mode_t mode);
static void block_release(struct gendisk *disk);
static int block_ioctl(struct block_device *bdev, blk_mode_t mode, unsigned int cmd, unsigned long arg);
static const struct block_device_operations operations = {
    .owner = THIS_MODULE, .open = block_open, .release = block_release, .ioctl = block_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = block_ioctl,
#endif
};
static bool geometry(struct dynblk_device *dev, struct super_block *sb)
{
	/* Btrfs owns member geometry and buffered COW/fsync, not a generic s_bdev. */
	if (!strcmp(sb->s_type->name, "btrfs"))
		return sb->s_magic == BTRFS_SUPER_MAGIC && sb->s_blocksize == DB_BLOCK &&
			sb->s_maxbytes >= dev->part_limit;
	return (!strcmp(sb->s_type->name, "ext4") ||
		!strcmp(sb->s_type->name, "ext2") || !strcmp(sb->s_type->name, "ntfs3") ||
		!strcmp(sb->s_type->name, "vfat") || !strcmp(sb->s_type->name, "exfat")) &&
		sb->s_bdev && sb->s_maxbytes >= DB_MIN_PART_LIMIT &&
		sb->s_blocksize >= 512 && sb->s_blocksize <= DB_BLOCK &&
		!(DB_BLOCK % sb->s_blocksize) &&
		bdev_logical_block_size(sb->s_bdev) >= 512 &&
		bdev_logical_block_size(sb->s_bdev) <= DB_BLOCK &&
		!(sb->s_blocksize % bdev_logical_block_size(sb->s_bdev)) &&
		bdev_physical_block_size(sb->s_bdev) >= 512 &&
		bdev_physical_block_size(sb->s_bdev) <= DB_BLOCK &&
		!(sb->s_blocksize % bdev_physical_block_size(sb->s_bdev)) &&
		!bdev_alignment_offset(sb->s_bdev) && sb->s_bdev->bd_disk->fops != &operations;
}

/* O_PATH inspection avoids ntfs_file_open's writable WOF decompression path. */
static int backing_attributes(struct file *f)
{
	struct fileattr fa = { .flags_valid = true };
	bool btrfs = !strcmp(file_inode(f)->i_sb->s_type->name, "btrfs");
	u32 flags;
	int ret;

	if (!btrfs && strcmp(file_inode(f)->i_sb->s_type->name, "ntfs3"))
		return 0;
	ret = vfs_fileattr_get(f->f_path.dentry, &fa);
	if (ret)
		return ret;
	if (!fa.flags_valid || (fa.flags & (FS_IMMUTABLE_FL | FS_APPEND_FL)))
		return -EOPNOTSUPP;
	if (btrfs)
		return fa.flags & FS_NOCOW_FL ? -EOPNOTSUPP : 0;
	if (fa.flags & (FS_COMPR_FL | FS_ENCRYPT_FL))
		return -EOPNOTSUPP;
	ret = vfs_getxattr(mnt_idmap(f->f_path.mnt), f->f_path.dentry,
			  "system.ntfs_attrib", &flags, sizeof(flags));
	if (ret != sizeof(flags))
		return ret < 0 ? ret : -EOPNOTSUPP;
	/* Sparse, reparse (including WOF/dedup), compressed, offline, encrypted. */
	return flags & 0x5e00U ? -EOPNOTSUPP : 0;
}

static int backing_inode(struct dynblk_device *dev, struct file *f)
{
	struct inode *inode = file_inode(f);

	if (!S_ISREG(inode->i_mode) || inode->i_nlink != 1 ||
	    is_idmapped_mnt(f->f_path.mnt) || !geometry(dev, inode->i_sb) ||
	    i_size_read(inode) > dev->part_limit)
		return -EINVAL;
	return backing_attributes(f);
}

static int admit(struct dynblk_device *dev, struct file *f)
{
	struct file_lock *lock;
	int ret;

	ret = backing_inode(dev, f);
	if (ret)
		return ret;
	lock = locks_alloc_lock();
	if (!lock)
		return -ENOMEM;
	lock->c.flc_owner = f;
	lock->c.flc_file = f;
	lock->c.flc_flags = FL_FLOCK;
	lock->c.flc_type = dev->read_only ? F_RDLCK : F_WRLCK;
	lock->c.flc_pid = task_tgid_nr(current);
	lock->fl_start = 0;
	lock->fl_end = OFFSET_MAX;
	ret = locks_lock_file_wait(f, lock);
	locks_free_lock(lock);
	return ret;
}

static int pin_directory(struct dynblk_device *dev)
{
	struct path path;
	char *parent, *slash, *p;
	int ret = -EINVAL;

	if (!dev->backing_filename || dev->backing_filename[0] != '/' || strlen(dev->backing_filename) >= PATH_MAX)
		return -EINVAL;
	parent = kstrdup(dev->backing_filename, GFP_KERNEL);
	if (!parent)
		return -ENOMEM;
	slash = strrchr(parent, '/');
	if (!de_name_ok(slash + 1))
		goto out;
	for (p = parent; p <= slash; p++) {
		char saved;

		if (*p != '/')
			continue;
		if (p != parent && (p[-1] == '/' ||
		    (p[-1] == '.' && (p - parent == 1 || p[-2] == '/' ||
		    (p[-2] == '.' && (p - parent == 2 || p[-3] == '/'))))))
			goto out;
		saved = p == parent ? parent[1] : *p;
		if (p == parent)
			parent[1] = 0;
		else
			*p = 0;
		ret = kern_path(parent, LOOKUP_DIRECTORY | LOOKUP_NO_SYMLINKS, &path);
		if (p == parent)
			parent[1] = saved;
		else
			*p = saved;
		if (ret)
			goto out;
		ret = -EOPNOTSUPP;
		if (is_idmapped_mnt(path.mnt)) {
			path_put(&path);
			goto out;
		}
		if (p == slash) {
			dev->directory = dentry_open(&path, O_RDONLY | O_DIRECTORY, dev->creation_cred);
			path_put(&path);
			if (IS_ERR(dev->directory)) {
				ret = PTR_ERR(dev->directory);
				dev->directory = NULL;
				goto out;
			}
			ret = geometry(dev, file_inode(dev->directory)->i_sb) ? 0 : -EOPNOTSUPP;
			if (!ret)
				ret = backing_attributes(dev->directory);
			break;
		}
		path_put(&path);
	}
out:
	kfree(parent);
	return ret;
}

static int setup_codec(struct dynblk_device *dev, const char *name)
{
	static const char * const names[] = {
		"none", "lz4", "lz4hc", "lzo", "lzo-rle", "zstd", "deflate", "842",
	};
	u32 i;

	for (i = 0; i < ARRAY_SIZE(names); i++)
		if (!strcmp(name, names[i]))
			break;
	if (i == ARRAY_SIZE(names))
		return -EINVAL;
	if (dev->codec) return strcmp(dev->codec_name, name) ? -EINVAL : 0;
    strscpy(dev->codec_name, name, sizeof(dev->codec_name));
	if (!i)
		return 0;
	dev->codec = crypto_alloc_comp(name, 0, 0);
	if (IS_ERR(dev->codec)) {
		int ret = PTR_ERR(dev->codec);

		dev->codec = NULL;
		return ret;
	}
	return 0;
}

static void *kd_alloc(void *ctx, size_t bytes)
{
    (void)ctx;
    return kvzalloc(bytes, GFP_NOIO);
}
static void kd_free(void *ctx, void *p) { (void)ctx; kvfree(p); }
static int kd_reserve_files(struct dynblk_device *dev, unsigned int need)
{
    struct file **p;
    unsigned int slots = dev->file_slots ? dev->file_slots : 8;
    if (need > DE_MAX_FILES) return -EFBIG;
    if (need <= dev->file_slots) return 0;
    while (slots < need) slots *= 2;
    if (slots > DE_MAX_FILES) slots = DE_MAX_FILES;
    p = kvcalloc(slots, sizeof(*p), GFP_NOIO);
    if (!p) return -ENOMEM;
    if (dev->file_slots) memcpy(p, dev->files, (size_t)dev->file_slots * sizeof(*p));
    kvfree(dev->files);
    dev->files = p; dev->file_slots = slots;
    return 0;
}
static int kd_open(void *ctx, unsigned int id, const char *name, bool create)
{
    struct dynblk_device *dev = ctx;
    struct file *f, *probe;
    const struct cred *old;
    unsigned int i;
    int ret, flags = dev->read_only ? O_RDONLY : O_RDWR;
    bool direct = dev->cache_mode == DE_NONE || dev->cache_mode == DE_DIRECTSYNC;
    if (id >= DE_MAX_FILES || !de_name_ok(name)) return -EINVAL;
    ret = kd_reserve_files(dev, id + 1);
    if (ret) return ret;
    if (dev->files[id]) return -EINVAL;
    if (create && dev->read_only) return -EROFS;
    if (direct && strcmp(file_inode(dev->directory)->i_sb->s_type->name, "ext4") &&
        strcmp(file_inode(dev->directory)->i_sb->s_type->name, "ext2")) return -EOPNOTSUPP;
    flags |= O_NOFOLLOW | O_LARGEFILE | O_NOATIME;
    if (direct) flags |= O_DIRECT;
    if (create) flags |= O_CREAT | O_EXCL;
    old = override_creds(dev->creation_cred);
    if (!create) {
        probe = file_open_root(&dev->directory->f_path, name, O_PATH | O_NOFOLLOW, 0);
        if (IS_ERR(probe)) { ret = PTR_ERR(probe); goto out; }
        ret = backing_inode(dev, probe);
        filp_close(probe, NULL);
        if (ret) goto out;
    }
    f = file_open_root(&dev->directory->f_path, name, flags, 0600);
    if (IS_ERR(f)) { ret = PTR_ERR(f); goto out; }
    dev->files[id] = f;
    ret = admit(dev, f);
    if (ret) goto out;
    for (i = 0; i < dev->file_slots; i++) {
        if (i != id && dev->files[i] && file_inode(dev->files[i]) == file_inode(f)) {
            ret = -EUCLEAN; goto out;
        }
    }
out:
    revert_creds(old);
    return ret;
}
static int kd_size(void *ctx, unsigned int id, db_u64 *size)
{
    struct dynblk_device *dev = ctx;
    if (id >= dev->file_slots || !dev->files[id]) return -EBADF;
    *size = i_size_read(file_inode(dev->files[id]));
    return 0;
}
static int kd_resize(void *ctx, unsigned int id, db_u64 size)
{
    struct dynblk_device *dev = ctx;
    const struct cred *old;
    int ret;
    if (dev->read_only) return -EROFS;
    if (id >= dev->file_slots || !dev->files[id] || size > DE_PART_LIMIT) return -EINVAL;
    old = override_creds(dev->creation_cred);
    ret = vfs_truncate(&dev->files[id]->f_path, size);
    revert_creds(old);
    return ret;
}
static ssize_t kd_file_io(struct file *file, void *buffer, size_t n, loff_t offset, bool write)
{
    if (write) return kernel_write(file, buffer, n, &offset);
    return kernel_read(file, buffer, n, &offset);
}
static int kd_io(void *ctx, unsigned int id, void *buffer, size_t bytes, db_u64 offset, bool write)
{
    struct dynblk_device *dev = ctx;
    struct file *file;
    u8 *buf = buffer;
    u64 oldsize, target;
    int ret = 0;
    if (id >= dev->file_slots || !dev->files[id] || offset > DE_PART_LIMIT || bytes > DE_PART_LIMIT - offset)
        return -EINVAL;
    if (write && dev->read_only) return -EROFS;
    file = dev->files[id];
    if (!dev->direct_buffer) {
        while (bytes) {
            ssize_t n = kd_file_io(file, buf, bytes, offset, write);
            if (n <= 0) return n < 0 ? (int)n : -EIO;
            bytes -= n; offset += n; buf += n;
        }
        return 0;
    }
    oldsize = i_size_read(file_inode(file));
    target = max_t(u64, oldsize, offset + bytes);
    while (bytes) {
        u64 base = round_down(offset, (u64)DE_PAGE);
        size_t within = offset % DE_PAGE, take = min_t(size_t, bytes, DE_GRAIN - within);
        size_t span = round_up(within + take, DE_PAGE);
        ssize_t got;
        memset(dev->direct_buffer, 0, span);
        if (!write || within || take != span) {
            got = kd_file_io(file, dev->direct_buffer, span, base, false);
            if (got < 0) return got;
            if (!write && got < within + take) return -EIO;
            if (write && got < (oldsize > base ? min_t(u64, oldsize - base, span) : 0)) return -EIO;
        }
        if (write) {
            memcpy(dev->direct_buffer + within, buf, take);
            got = kd_file_io(file, dev->direct_buffer, span, base, true);
            if (got < 0) return got;
            if (got != span) return -EIO;
        } else memcpy(buf, dev->direct_buffer + within, take);
        bytes -= take; offset += take; buf += take;
    }
    /* Sector-aligned direct RMW must not extend a short descriptor on CID updates. */
    if (write && i_size_read(file_inode(file)) > target) ret = kd_resize(dev, id, target);
    return ret;
}
static int kd_sync(void *ctx, unsigned int id)
{
    struct dynblk_device *dev = ctx;
    if (dev->read_only) return 0;
    if (id >= dev->file_slots || !dev->files[id]) return -EBADF;
    return vfs_fsync(dev->files[id], 0);
}
static int kd_punch(void *ctx, unsigned int id, db_u64 off, db_u64 bytes)
{
    struct dynblk_device *dev = ctx;
    const struct cred *old;
    int ret;
    if (dev->read_only) return -EROFS;
    if (id >= dev->file_slots || !dev->files[id] || off % DE_PAGE || bytes % DE_PAGE ||
        off > DE_PART_LIMIT || bytes > DE_PART_LIMIT - off) return -EINVAL;
    old = override_creds(dev->creation_cred);
    ret = vfs_fallocate(dev->files[id], FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, off, bytes);
    revert_creds(old);
    return ret;
}
static int kd_sync_dir(void *ctx)
{
    struct dynblk_device *dev = ctx;
    return dev->read_only || dev->cache_mode == DE_UNSAFE ? 0 : vfs_fsync(dev->directory, 0);
}
static int kd_codec(void *ctx, const char *name, bool encode, const void *src,
                    unsigned int bytes, void *dst, unsigned int *outlen)
{
    struct dynblk_device *dev = ctx;
    int ret = setup_codec(dev, name);
    if (ret) return ret;
    if (!dev->codec) return -EINVAL;
    return encode ? crypto_comp_compress(dev->codec, src, bytes, dst, outlen) :
        crypto_comp_decompress(dev->codec, src, bytes, dst, outlen);
}
static void close_storage(struct dynblk_device *dev)
{
    unsigned int i;
    de_destroy(&dev->engine);
    for (i = 0; i < dev->file_slots; i++) if (dev->files[i]) {
        get_file(dev->files[i]);
        filp_close(dev->files[i], NULL);
        __fput_sync(dev->files[i]);
    }
    if (dev->directory) filp_close(dev->directory, NULL);
    if (dev->creation_cred) put_cred(dev->creation_cred);
    if (dev->codec) crypto_free_comp(dev->codec);
    kvfree(dev->files); kvfree(dev->raw); kfree(dev->direct_buffer); kfree(dev->backing_filename);
}

struct db_request { struct work_struct work; };
static void request_copy(struct dynblk_device *dev, struct request *rq, bool to_request)
{
	struct req_iterator iter;
	struct bio_vec bv;
	u32 copied = 0;

	rq_for_each_segment(bv, rq, iter) {
		u32 done = 0;

		while (done < bv.bv_len) {
			u32 offset = bv.bv_offset + done, within = offset % PAGE_SIZE;
			u32 len = min_t(u32, bv.bv_len - done, PAGE_SIZE - within);
			void *mapped = kmap_local_page(nth_page(bv.bv_page, offset / PAGE_SIZE));

			if (to_request)
				memcpy(mapped + within, dev->raw + copied, len);
			else
				memcpy(dev->raw + copied, mapped + within, len);
			kunmap_local(mapped);
			done += len;
			copied += len;
		}
	}
}

static int transfer(struct dynblk_device *dev, struct request *rq)
{
    struct de_engine *e = &dev->engine;
    u64 pos = (u64)blk_rq_pos(rq) << SECTOR_SHIFT;
    unsigned int len = blk_rq_bytes(rq), op = req_op(rq);
    unsigned int alignment = e->format == DE_NATIVE ? DE_PAGE : 512;
    int ret;
    if (rq->cmd_flags & (REQ_NOWAIT | REQ_POLLED | REQ_ATOMIC)) return -EOPNOTSUPP;
    if (op == REQ_OP_FLUSH) return len ? -EINVAL : de_flush(e);
    if (op != REQ_OP_READ && op != REQ_OP_WRITE && op != REQ_OP_DISCARD) return -EOPNOTSUPP;
    if (blk_rq_pos(rq) > e->capacity >> 9 || pos > e->capacity ||
        len > e->capacity - pos || len > DB_IO || pos % alignment || len % alignment) return -EIO;
    if (op != REQ_OP_READ && dev->read_only) return -EROFS;
    if (op != REQ_OP_READ && dev->fenced) return -EIO;
    if (rq->cmd_flags & REQ_PREFLUSH) {
        ret = de_flush(e);
        if (ret) return ret;
    }
    if (op == REQ_OP_READ) {
        ret = de_read(e, pos, dev->raw, len);
        if (!ret) request_copy(dev, rq, true);
        return ret;
    }
    if (op == REQ_OP_DISCARD) ret = de_discard(e, pos, len);
    else {
        request_copy(dev, rq, false);
        ret = de_write(e, pos, dev->raw, len);
    }
    if (!ret && rq->cmd_flags & REQ_FUA) ret = de_flush(e);
    if (!ret && !READ_ONCE(dev->detaching) &&
        (e->cache_mode == DE_WRITEBACK || e->cache_mode == DE_NONE || e->cache_mode == DE_UNSAFE))
        queue_delayed_work(dev->worker, &dev->flush_work, HZ);
    return ret;
}
static void background_flush(struct work_struct *work)
{
    struct dynblk_device *dev = container_of(to_delayed_work(work), struct dynblk_device, flush_work);
    unsigned int noio = memalloc_noio_save();
    int ret;
    mutex_lock(&dev->state_lock);
    ret = de_flush(&dev->engine);
    if (ret) {
        dev->fenced = true;
        pr_err("dynblk%d: background flush failed: %d; writes fenced\n", dev->index, ret);
    }
    mutex_unlock(&dev->state_lock);
    memalloc_noio_restore(noio);
}
static void process_request(struct work_struct *work)
{
    struct db_request *pdu = container_of(work, struct db_request, work);
    struct request *rq = blk_mq_rq_from_pdu(pdu);
    struct dynblk_device *dev = rq->q->queuedata;
    unsigned int noio = memalloc_noio_save();
    int ret;
    mutex_lock(&dev->state_lock);
    ret = transfer(dev, rq);
    if (dev->engine.failed || ret == -EUCLEAN || ret == -EIO) {
        dev->fenced = true;
        dev->engine.failed = true;
    }
    mutex_unlock(&dev->state_lock);
    memalloc_noio_restore(noio);
    blk_mq_end_request(rq, errno_to_blk_status(ret));
}

static blk_status_t queue_request(struct blk_mq_hw_ctx *hctx,
				 const struct blk_mq_queue_data *bd)
{
	struct db_request *pdu = blk_mq_rq_to_pdu(bd->rq);
	struct dynblk_device *dev = bd->rq->q->queuedata;

	blk_mq_start_request(bd->rq);
	queue_work(dev->worker, &pdu->work);
	return BLK_STS_OK;
}
static int init_request(struct blk_mq_tag_set *set, struct request *rq,
			unsigned int hctx, unsigned int node)
{
	struct db_request *pdu = blk_mq_rq_to_pdu(rq);

	INIT_WORK(&pdu->work, process_request);
	return 0;
}
static enum blk_eh_timer_return request_timeout(struct request *rq)
{
	return BLK_EH_RESET_TIMER;
}
static const struct blk_mq_ops mq_ops = {
    .queue_rq = queue_request, .init_request = init_request, .timeout = request_timeout,
};
static int block_open(struct gendisk *disk, blk_mode_t mode)
{
	struct dynblk_device *dev = disk->private_data;
	int ret = 0;

	(void)mode;
	mutex_lock(&dev->lifecycle_lock);
	if (dev->detaching)
		ret = -ENXIO;
	else
		dev->openers++;
	mutex_unlock(&dev->lifecycle_lock);
	return ret;
}
static void block_release(struct gendisk *disk)
{
	struct dynblk_device *dev = disk->private_data;

	mutex_lock(&dev->lifecycle_lock);
	WARN_ON_ONCE(!dev->openers);
	if (dev->openers)
		dev->openers--;
	if (!dev->openers && dev->autoclear && !dev->detaching &&
	    !dev->autoclear_pending) {
		/* Release runs inside the block layer: teardown must run elsewhere.
		 * Pending work excludes explicit detach and owns an extra module pin. */
		dev->autoclear_pending = true;
		__module_get(THIS_MODULE);
		queue_work(autoclear_queue, &dev->autoclear_work);
	}
	mutex_unlock(&dev->lifecycle_lock);
}

static int reclaim_ioctl(struct dynblk_device *dev, blk_mode_t mode, unsigned long arg)
{
    struct dynblk_reclaim request;
    struct de_reclaim step = {0};
    u64 punched, truncated;
    unsigned int noio;
    int ret;
    if (!ns_capable(&init_user_ns, CAP_SYS_ADMIN) || !(mode & BLK_OPEN_WRITE)) return -EPERM;
    if (copy_from_user(&request, (void __user *)arg, sizeof(request))) return -EFAULT;
    if (request.version != DYNBLK_ABI_VERSION ||
        request.flags & ~(DYNBLK_RECLAIM_COMPACT | DYNBLK_RECLAIM_ZEROES)) return -EINVAL;
    if (request.cookie != dev->cookie) return -ESTALE;
    step.part = request.part; step.phase = request.phase; step.cursor = request.cursor;
    step.flags = request.flags;
    noio = memalloc_noio_save();
    mutex_lock(&dev->state_lock);
    punched = dev->engine.punched_bytes; truncated = dev->engine.truncated_bytes;
    ret = de_reclaim_step(&dev->engine, &step);
    if (dev->engine.failed) dev->fenced = true;
    request.part = step.part; request.phase = step.phase; request.cursor = step.cursor;
    request.scanned_bytes = step.scanned_bytes; request.moved_bytes = step.moved_bytes;
    request.unmapped_bytes = step.unmapped_bytes;
    request.punch_bytes = dev->engine.punched_bytes - punched;
    request.truncated_bytes = dev->engine.truncated_bytes - truncated;
    request.done = step.done; request.method = step.method;
    mutex_unlock(&dev->state_lock);
    memalloc_noio_restore(noio);
    if (ret) return ret;
    return copy_to_user((void __user *)arg, &request, sizeof(request)) ? -EFAULT : 0;
}

static int block_ioctl(struct block_device *bdev, blk_mode_t mode,
                       unsigned int cmd, unsigned long arg)
{
    struct dynblk_device *dev = bdev->bd_disk->private_data;
    struct de_engine *e = &dev->engine;
    struct dynblk_status status = {0};
    u64 bytes = 0;
    unsigned int i, noio;
    int ret = 0;
    if (bdev_is_partition(bdev)) return -ENOTTY;
    if (cmd == DYNBLK_RECLAIM) return reclaim_ioctl(dev, mode, arg);
    if (cmd == DYNBLK_COOKIE)
        return copy_to_user((void __user *)arg, &dev->cookie, sizeof(dev->cookie)) ? -EFAULT : 0;
    if (cmd == DYNBLK_AUTOCLEAR) {
        if (!ns_capable(&init_user_ns, CAP_SYS_ADMIN)) return -EPERM;
        if (copy_from_user(&bytes, (void __user *)arg, sizeof(bytes))) return -EFAULT;
        mutex_lock(&dev->lifecycle_lock);
        if (bytes != dev->cookie) ret = -ESTALE;
        else if (dev->detaching) ret = -EBUSY;
        else dev->autoclear = true;
        mutex_unlock(&dev->lifecycle_lock);
        return ret;
    }
    if (cmd != DYNBLK_GET && cmd != DYNBLK_GROW) return -ENOTTY;
    if (cmd == DYNBLK_GROW) {
        if (!ns_capable(&init_user_ns, CAP_SYS_ADMIN) || !(mode & BLK_OPEN_WRITE)) return -EPERM;
        if (copy_from_user(&bytes, (void __user *)arg, sizeof(bytes))) return -EFAULT;
    }
    noio = memalloc_noio_save();
    mutex_lock(&dev->state_lock);
    if (cmd == DYNBLK_GET) {
        status.version = DYNBLK_ABI_VERSION;
        status.flags = (dev->fenced ? DYNBLK_STATUS_FENCED : 0) |
            (dev->read_only ? DYNBLK_STATUS_READ_ONLY : 0) |
            (READ_ONCE(dev->autoclear) ? DYNBLK_STATUS_AUTOCLEAR : 0);
        memcpy(status.uuid, e->uuid, 16); memcpy(status.algorithm, e->codec, 16);
        status.capacity = e->capacity;
        status.original_bytes = min_t(u64, e->capacity, e->mapped_grains * DE_GRAIN);
        status.stored_bytes = e->stored_bytes; status.generation = e->generation;
        status.part_limit = e->part_limit; status.active_parts = e->nr_extents;
        status.block_size = e->format == DE_NATIVE ? DE_PAGE : 512;
        status.max_parts = DE_MAX_PARTS; status.tree_height = 2;
        status.map_memory_bytes = (u64)e->nr_cache * DE_PAGE;
        for (i = 0; i < e->nr_extents; i++)
            status.map_memory_bytes += e->extents[i].directories * 4ULL * (e->extents[i].backup ? 2 : 1);
        status.max_capacity = de_capacity_limit(e->format, e->part_limit); status.compression_region_bytes = DE_GRAIN;
        status.storage_format = e->format; status.cache_mode = e->cache_mode;
        status.engine_memory_bytes = de_memory(e) + sizeof(*dev) - sizeof(*e) + (u64)dev->file_slots * sizeof(*dev->files) + DB_IO +
            (dev->direct_buffer ? DE_GRAIN : 0);
        status.cache_hits = e->cache_hits; status.cache_misses = e->cache_misses; status.flush_count = e->flushes;
        status.capabilities = DYNBLK_CAP_GROW | DYNBLK_CAP_RECLAIM |
            (e->format == DE_NATIVE ? DYNBLK_CAP_COMPRESSION : 0);
        for (i = 0; i < dev->file_slots; i++)
            if (dev->files[i]) status.physical_bytes += i_size_read(file_inode(dev->files[i]));
    } else {
        ret = de_grow(e, bytes);
        if (e->failed) dev->fenced = true;
        set_capacity_and_notify(dev->disk, e->capacity >> 9);
    }
    mutex_unlock(&dev->state_lock);
    memalloc_noio_restore(noio);
    if (!ret && cmd == DYNBLK_GET && copy_to_user((void __user *)arg, &status, sizeof(status))) ret = -EFAULT;
    return ret;
}


static int validate_attach(struct dynblk_attach *r)
{
    if (r->version != DYNBLK_ABI_VERSION || r->device != -1 || r->cookie ||
        r->flags & ~(DYNBLK_ATTACH_CREATE | DYNBLK_ATTACH_READ_ONLY) ||
        r->map_memory_mb > DE_CACHE_MAX_MB || r->storage_format > DE_VMDK ||
        r->cache_mode > DE_UNSAFE || !r->path[0] ||
        !memchr(r->path, 0, sizeof(r->path))) return -EINVAL;
    if (!(r->flags & DYNBLK_ATTACH_CREATE))
        return r->size_bytes || r->part_limit_bytes ||
            memchr_inv(r->algorithm, 0, sizeof(r->algorithm)) ? -EINVAL : 0;
    if (r->flags & DYNBLK_ATTACH_READ_ONLY) return -EINVAL;
    if (!r->storage_format) r->storage_format = DE_NATIVE;
    if (!r->size_bytes) r->size_bytes = 16ULL << 30;
    if (!r->part_limit_bytes) r->part_limit_bytes = DE_PART_LIMIT;
    if (r->size_bytes > de_capacity_limit(r->storage_format, r->part_limit_bytes) || r->size_bytes % DE_PAGE ||
        r->part_limit_bytes < (1ULL << 20) || r->part_limit_bytes > DE_PART_LIMIT ||
        r->part_limit_bytes % DE_PAGE || !memchr(r->algorithm, 0, sizeof(r->algorithm)) ||
        !de_codec_valid(r->algorithm)) return -EINVAL;
    if (r->storage_format == DE_VMDK && (strcmp(r->algorithm, "none") ||
        r->part_limit_bytes != DE_PART_LIMIT)) return -EOPNOTSUPP;
    return 0;
}
static int attach_device(struct dynblk_attach *request)
{
    struct queue_limits limits = {
        .physical_block_size = DE_PAGE,
        .max_hw_sectors = DB_IO >> 9,
        .max_segments = DB_IO / 512,
        .max_hw_discard_sectors = DB_IO >> 9,
        .discard_granularity = DE_PAGE,
    };
    struct dynblk_device *dev;
    struct de_io io;
    const char *name;
    unsigned int noio;
    int index, ret = validate_attach(request);
    if (ret) return ret;
    if (!ns_capable(&init_user_ns, CAP_SYS_ADMIN) || current_user_ns() != &init_user_ns) return -EPERM;
    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev) return -ENOMEM;
    index = ida_alloc_max(&device_ids, DYNBLK_MAX_DEVICES - 1, GFP_KERNEL);
    if (index < 0) { kfree(dev); return index; }
    dev->index = index;
    dev->creating = !!(request->flags & DYNBLK_ATTACH_CREATE);
    dev->read_only = !!(request->flags & DYNBLK_ATTACH_READ_ONLY);
    dev->map_memory_mb = request->map_memory_mb;
    dev->cache_mode = request->cache_mode;
    dev->part_limit = DE_PART_LIMIT;
    INIT_WORK(&dev->autoclear_work, autoclear_device);
    INIT_DELAYED_WORK(&dev->flush_work, background_flush);
    mutex_init(&dev->state_lock); mutex_init(&dev->lifecycle_lock);
    do { get_random_bytes(&dev->cookie, sizeof(dev->cookie)); } while (!dev->cookie);
    dev->creation_cred = get_current_cred();
    dev->backing_filename = kstrdup(request->path, GFP_KERNEL);
    dev->raw = kvzalloc(DB_IO, GFP_KERNEL);
    if (dev->cache_mode == DE_NONE || dev->cache_mode == DE_DIRECTSYNC)
        dev->direct_buffer = kmalloc(DE_GRAIN, GFP_KERNEL);
    if (!dev->backing_filename || !dev->raw ||
        ((dev->cache_mode == DE_NONE || dev->cache_mode == DE_DIRECTSYNC) && !dev->direct_buffer)) {
        ret = -ENOMEM; goto fail_storage;
    }
    ret = pin_directory(dev);
    if (ret) goto fail_storage;
    if (dev->creating) {
        ret = setup_codec(dev, request->algorithm);
        if (ret) goto fail_storage;
    }
    io = (struct de_io){.ctx = dev, .alloc = kd_alloc, .free = kd_free, .open = kd_open,
        .io = kd_io, .size = kd_size, .resize = kd_resize, .sync = kd_sync,
        .sync_dir = kd_sync_dir, .codec = kd_codec,
        .punch = !strcmp(file_inode(dev->directory)->i_sb->s_type->name, "ext4") ? kd_punch : NULL};
    noio = memalloc_noio_save();
    ret = de_init(&dev->engine, &io, dev->map_memory_mb, dev->read_only, dev->cache_mode);
    if (!ret) {
        generate_random_uuid(dev->engine.uuid);
        name = strrchr(dev->backing_filename, '/') + 1;
        if (dev->creating) ret = de_create(&dev->engine, name, request->storage_format,
            request->size_bytes, request->part_limit_bytes, request->algorithm, dev->engine.uuid);
        else ret = de_open(&dev->engine, name, request->storage_format);
    }
    if (!ret) ret = setup_codec(dev, dev->engine.codec);
    memalloc_noio_restore(noio);
    if (ret) goto fail_storage;
    limits.logical_block_size = dev->engine.format == DE_NATIVE ? DE_PAGE : 512;
    if (dev->cache_mode != DE_WRITETHROUGH && dev->cache_mode != DE_DIRECTSYNC)
        limits.features = BLK_FEAT_WRITE_CACHE | BLK_FEAT_FUA;
    dev->worker = alloc_ordered_workqueue("dynblk%d", WQ_MEM_RECLAIM, dev->index);
    if (!dev->worker) { ret = -ENOMEM; goto fail_storage; }
    dev->tags.ops = &mq_ops; dev->tags.nr_hw_queues = 1; dev->tags.queue_depth = 128;
    dev->tags.numa_node = NUMA_NO_NODE; dev->tags.cmd_size = sizeof(struct db_request);
    dev->tags.flags = BLK_MQ_F_SHOULD_MERGE;
    ret = blk_mq_alloc_tag_set(&dev->tags);
    if (ret) goto fail_worker;
    dev->disk = blk_mq_alloc_disk(&dev->tags, &limits, dev);
    if (IS_ERR(dev->disk)) { ret = PTR_ERR(dev->disk); goto fail_tags; }
    dev->disk->fops = &operations; dev->disk->private_data = dev;
    snprintf(dev->disk->disk_name, DISK_NAME_LEN, "dynblk%d", dev->index);
    set_capacity(dev->disk, dev->engine.capacity >> 9);
    set_disk_ro(dev->disk, dev->read_only);
    ret = add_disk(dev->disk);
    if (ret) goto fail_disk;
    mutex_lock(&devices_lock);
    devices[dev->index] = dev;
    mutex_unlock(&devices_lock);
    __module_get(THIS_MODULE);
    request->device = dev->index; request->cookie = dev->cookie;
    pr_info("dynblk%d: %s format 1, %llu bytes, %s, cache=%s\n", dev->index,
        de_format_name(dev->engine.format), dev->engine.capacity, dev->engine.codec,
        de_cache_name(dev->cache_mode));
    return 0;
fail_disk:
    put_disk(dev->disk);
fail_tags:
    blk_mq_free_tag_set(&dev->tags);
fail_worker:
    destroy_workqueue(dev->worker);
fail_storage:
    pr_err("dynblk%d: attachment failed: %d; partial files preserved\n", dev->index, ret);
    close_storage(dev); ida_free(&device_ids, dev->index); kfree(dev);
    return ret;
}

static int detach_device(u32 index, u64 cookie, bool automatic)
{
	struct dynblk_device *dev;
	unsigned int noio;
	int sync_error = 0;

	if (index >= DYNBLK_MAX_DEVICES || !cookie)
		return -ENODEV;
	mutex_lock(&devices_lock);
	dev = devices[index];
	if (!dev) {
		mutex_unlock(&devices_lock);
		return -ENODEV;
	}
	if (dev->cookie != cookie) {
		mutex_unlock(&devices_lock);
		return -ESTALE;
	}
	mutex_lock(&dev->lifecycle_lock);
	if (automatic)
		dev->autoclear_pending = false;
	if (dev->detaching || dev->openers || dev->autoclear_pending) {
		mutex_unlock(&dev->lifecycle_lock);
		mutex_unlock(&devices_lock);
		return -EBUSY;
	}
	dev->detaching = true;
	mutex_unlock(&dev->lifecycle_lock);
	mutex_unlock(&devices_lock);

	cancel_delayed_work_sync(&dev->flush_work);
	flush_workqueue(dev->worker);
	cancel_delayed_work_sync(&dev->flush_work);
	noio = memalloc_noio_save();
	mutex_lock(&dev->state_lock);
	if (!dev->fenced) {
		sync_error = de_flush(&dev->engine);
		if (sync_error) {
			mutex_unlock(&dev->state_lock);
			memalloc_noio_restore(noio);
			mutex_lock(&dev->lifecycle_lock);
			dev->detaching = false;
			mutex_unlock(&dev->lifecycle_lock);
			return sync_error;
		}
	} else {
		sync_error = -EIO;
	}
	mutex_unlock(&dev->state_lock);
	memalloc_noio_restore(noio);
	if (sync_error)
		pr_warn("dynblk%u: detaching fenced device without final sync; recovery required on reattach\n",
			index);

	mutex_lock(&devices_lock);
	if (devices[index] != dev || dev->cookie != cookie) {
		mutex_unlock(&devices_lock);
		return -EUCLEAN;
	}
	devices[index] = NULL;
	mutex_unlock(&devices_lock);

	del_gendisk(dev->disk);
	put_disk(dev->disk);
	blk_mq_free_tag_set(&dev->tags);
	destroy_workqueue(dev->worker);
	close_storage(dev);
	ida_free(&device_ids, dev->index);
	pr_info("dynblk%u: detached\n", index);
	kfree(dev);
	module_put(THIS_MODULE);
	return 0;
}
static void autoclear_device(struct work_struct *work)
{
	struct dynblk_device *dev = container_of(work, struct dynblk_device, autoclear_work);
	u32 index = dev->index;
	u64 cookie = dev->cookie;
	int ret;

	/* detach_device can free dev, so do not dereference it after this call. */
	ret = detach_device(index, cookie, true);
	if (ret && ret != -EBUSY)
		pr_warn("dynblk%u: automatic detach failed: %d; device retained\n", index, ret);
	module_put(THIS_MODULE);
}
static long control_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *user = (void __user *)arg;
	int ret;

	(void)file;
	if (!ns_capable(&init_user_ns, CAP_SYS_ADMIN) || current_user_ns() != &init_user_ns)
		return -EPERM;
	if (cmd == DYNBLK_ATTACH) {
		struct dynblk_attach *request = memdup_user(user, sizeof(*request));

		if (IS_ERR(request))
			return PTR_ERR(request);
		ret = attach_device(request);
		if (!ret && copy_to_user(user, request, sizeof(*request))) {
			detach_device(request->device, request->cookie, false);
			ret = -EFAULT;
		}
		kfree(request);
		return ret;
	}
	if (cmd == DYNBLK_DETACH) {
		struct dynblk_detach request;

		if (copy_from_user(&request, user, sizeof(request)))
			return -EFAULT;
		if (request.version != DYNBLK_ABI_VERSION || !request.cookie)
			return -EINVAL;
		return detach_device(request.device, request.cookie, false);
	}
	return -ENOTTY;
}
static const struct file_operations control_operations = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = control_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = control_ioctl,
#endif
	.llseek = noop_llseek,
};

static struct miscdevice control_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "dynblk-control",
	.fops = &control_operations,
	.mode = 0600,
};

static int __init dynblk_init(void)
{
    int ret;
    BUILD_BUG_ON(sizeof(struct dynblk_status) != 176);
    BUILD_BUG_ON(sizeof(struct dynblk_attach) >= (1U << _IOC_SIZEBITS));
    if (!IS_ENABLED(CONFIG_FILE_LOCKING) || PAGE_SIZE != DB_BLOCK) return -EOPNOTSUPP;
    autoclear_queue = alloc_workqueue("dynblk-autoclear", WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
    if (!autoclear_queue) return -ENOMEM;
    ret = misc_register(&control_device);
    if (ret) destroy_workqueue(autoclear_queue);
    return ret;
}
static void __exit dynblk_exit(void)
{
	u32 index;

	misc_deregister(&control_device);
	/* An autoclear callback may have just dropped the last module pin;
	 * wait for its epilogue before freeing this module's executable text. */
	destroy_workqueue(autoclear_queue);
	for (index = 0; index < DYNBLK_MAX_DEVICES; index++)
		WARN_ON_ONCE(devices[index]);
	ida_destroy(&device_ids);
}

module_init(dynblk_init);
module_exit(dynblk_exit);
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0.0");
MODULE_DESCRIPTION("Compressed sparse DynBlk format 1 and split VMDK block devices");
