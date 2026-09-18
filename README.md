# DynBlk

DynBlk is a Linux 6.12 file-backed block driver with two storage backends:
**native compressed format 1** and **standard split sparse VMDK**. Both expose
ordinary `/dev/dynblkN` disks and support partitions, read-only attachment,
selectable caching, growth and `mount.dynblk`. No FUSE server, NBD process,
Python runtime or virtual machine is needed to use an attached disk.

## Compatibility

Native images use magic `DBSPRS01` and disk format number **1**. Unsupported
layouts are rejected, never automatically converted or overwritten. Install
matching CLI and kernel module versions together.

VMDK support is limited to `twoGbMaxExtentSparse`, with 64-KiB grains, 512-entry
grain tables, ordinary sparse extents and no parent/snapshot chain. The primary
file is the text descriptor; supply it, not an individual `-sNNN.vmdk` extent.
Unsupported subformats and compression combinations fail explicitly. Read/write
interoperability is checked with QEMU image tools, not certified for every
VMware or VirtualBox release.

## Create and attach

Create the parent directory first; these examples require root. Mutating CLI
commands are previews unless `--execute` is present. No command formats an
upper filesystem or automatically converts an existing container.

```sh
dynblk create /srv/native/volume000.db --size 16GiB \
    --format dynblk --compression lz4 --cache writeback --execute
dynblk create /srv/portable/disk.vmdk --size 16GiB \
    --format vmdk --subformat twoGbMaxExtentSparse --cache none --execute
# Each command prints its assigned /dev/dynblkN.
dynblk status /dev/dynblk0 --json
dynblk unload /dev/dynblk0 --execute
dynblk load /srv/native/volume000.db --cache writethrough --execute
```

Creation defaults: native format, 16 GiB virtual capacity, `compression=none`,
`cache=writeback`. Load detects the format unless `--format` selects one.
`--read-only` is load-only. `--module /absolute/path/dynblk.ko` can use a matching
module explicitly; normally the CLI uses `modprobe dynblk`.

The native backend uses fixed logical ranges, normally 1 GiB per part, with
up to 4000 MiB physical file length per part. Metadata files for all declared
ranges are created initially; data allocation is sparse. The default filename
sequence is `volume000.db`, `volume001.db`, etc. Other names use `.001` suffixes.
`--part-size` changes the native physical cap and consequently its logical span.
The VMDK backend uses up to 2 GiB logical ranges and standard `-sNNN.vmdk` names.
The old 512-GiB ceiling is removed. Extent/file vectors are allocated for the
actual image and expand on growth, not for the maximum supported disk.
A resource guard allows up to 65536 extents: with standard geometry this means
64 TiB for native DynBlk or an addressing ceiling of 128 TiB for split VMDK.
VMDK also requires a descriptor smaller than 1 MiB; its effective extent count
therefore depends on filename lengths. Small native part caps reduce the limit.
Use `dynblk limits --format dynblk --json` (or `--format vmdk`) to query geometry
limits without root, devices, or module loading. Neither capacity nor file
length is a reservation of physical disk space. File descriptor and memory
limits still apply. New `DBSPRS01` images retain format 1 and remain compatible
with smaller images created by this layout; no capacity-only conversion is needed.

## Caching and memory

`--cache MODE` is a per-attachment choice, independent of the storage format.

| Mode | Host page cache | Ordinary writes durable at completion | Flush/FUA |
|---|---|---|---|
| writeback (default) | yes | not required | honored |
| writethrough | yes | yes | honored |
| none | bypassed | not required | honored |
| directsync | bypassed | yes | honored |
| unsafe | yes | not required | durability deliberately omitted |

Direct modes currently require an ext4 or ext2 backing filesystem. Unsupported
combinations fail rather than silently selecting buffered I/O. Aligned bounce
buffers handle short metadata and descriptor accesses. `unsafe` is explicitly
opt-in and can lose or corrupt data after a crash, including flushed guest data.

`--map-memory-mb N` sets the metadata-page cache (1..64 MiB, default 1 MiB).
It is no longer a fraction of host RAM or a limit on how much data can be mapped.
The driver also needs small directories, allocation state and scratch buffers.
`engine_memory_bytes` reports accounted buffers, not total kernel/slab memory;
page cache, compression-provider allocations and block-layer allocations are
additional. Growing/filling a disk does not require a full resident block map.

Ordinary raw overwrites stay in place. Native compressed grains are replaced,
with new payload synchronized before mappings and old space reused afterward.
A partial update of compressed data currently reads/recompresses its 64-KiB
grain; this is not the old independent 4-KiB blob representation. No unbounded
write cache or per-write COW radix-tree publication remains.

## Mount existing filesystems

```sh
mount -t dynblk /srv/native/volume000.db /mnt/data \
    -o inner-fstype=ext4,cache=writeback
umount /mnt/data
mount -t dynblk /srv/portable/disk.vmdk /mnt/data \
    -o format=vmdk,partition=1,inner-fstype=ext4,ro
```

The target and filesystem must already exist. The helper does not create,
format, fsck, resize, unlock LUKS or choose a partition implicitly. Omit
`inner-fstype` to let the inner mount program probe a whole-disk filesystem.
Normal filesystem options are forwarded without invoking a shell. The helper
is root-only, not setuid; `user`/`users` mounts and helper-driven remount/bind
operations are not supported. `-f` checks arguments without attaching anything.

The helper creates an autoclear attachment. Open descriptors, mounted partitions,
bind mounts and references surviving lazy unmount delay last-close teardown.
A plain `dynblk load` has manual lifetime. Cleanup uses the attachment cookie,
not just a potentially reused device number. Failed final synchronization is
reported; a fenced device may require explicit unload.

An fstab entry may use type `dynblk`, `noauto`, and the same helper options.
With systemd, order the mount after the filesystem containing the image.
Read-only attaches use shared locks and never repair/truncate/reclaim storage.
A read-only inner ext4 may require explicit `noload` if journal recovery is
needed; DynBlk will not write through a read-only attachment to perform it.

## Growth, discard and checking

`dynblk grow DEVICE SIZE --execute` grows the virtual disk, not its filesystem;
shrinking is rejected. VMDK growth appends extents and rewrites the descriptor.
Both native and split VMDK discard release complete 64-KiB grains and reuse
freed storage. After the mapping barrier, ext4 backing files automatically have
unreferenced ranges hole-punched; completely free file tails can be truncated.
These automatic actions NEVER move live data. In particular, exFAT has no
fallback that relocates data, either during discard, ordinary I/O or attach.
Partial-grain discard still zeroes the requested bytes and may retain allocation.

```sh
# The inner filesystem must first tell the device which blocks are unused.
fstrim /mnt/inner-filesystem
# Non-moving cleanup only; safe to use on ext4 or exFAT:
dynblk reclaim /dev/dynblk0 --execute --json
# Explicit, potentially write-intensive in-place compaction, also for exFAT:
dynblk reclaim /dev/dynblk0 --compact --execute --json
```

`--compact` is the ONLY way to request relocation. It copies already encoded
native objects or raw VMDK grains into earlier free runs, publishes their new
mappings, then frees/truncates the old placement. No second image, conversion,
filesystem resize or format change is involved. The volume may stay mounted.
Normal I/O is serialized with each bounded step and runs between steps; initial
usage reconstruction scans one part's metadata, so latency is not strictly timed.
One pass need not eliminate every hole, especially with concurrent writes or
variable-size compressed objects. Live data never cross logical extent boundaries.

`--scan-zeroes` additionally reads mapped grains and unmaps all-zero grains;
it does not infer filesystem free space and is not run automatically. Without
`--execute`, reclaim is a dry-run that does not open the device or estimate its
free space. `--max-steps N` bounds work; `complete:false` means the scan stopped
early (a subsequent invocation starts a fresh pass). Read-only and `unsafe`
attachments reject reclaim. Stop/retry leaves completed steps in place.

Counters distinguish moved payload, unmapped logical bytes, requested punch
ranges and reduced file lengths. Punch/truncation counts are NOT measured
allocated-byte savings: use `du`/allocated blocks and lower-filesystem free
space to verify actual returns. A kernel-side evictable usage bitmap adds at
most 128 KiB at the standard per-file cap, not one allocation per extent.
The static initrd CLI exposes the same command; no automatic startup compaction
or forced LUKS discard passthrough is installed.

`inspect PATH --metadata-only [--json]` validates headers, directories and live
mapping bounds/overlaps without decoding payloads. `check PATH [--json]` also
reads all mapped payloads and verifies native compressed CRCs/decompression.
There are no stored checksums for raw native or ordinary VMDK data, so a full
check establishes readability, not detection of every possible bit flip.
Both commands are read-only and cooperate with attachment locks.

Attach scans mapping metadata, not all payload data. A writable native attach
reconstructs its allocation bitmap one part at a time. This bounds memory but
still makes attach time depend on declared capacity. The current format has no
transaction journal or automatic repair of torn mapping entries; malformed
metadata is rejected. Completed flushes rely on the backing filesystem/device
honoring synchronization. Tests are evidence of the covered cases, not proof
against every hardware failure. See [FORMAT.md](FORMAT.md) for exact ordering.

Codecs: none, lz4, lz4hc, lzo, lzo-rle, zstd, deflate, 842; kernel providers must
exist and errors are not replaced with another codec. The offline checker has
built-in LZO/LZO-RLE and optional dynamically loaded LZ4/Zstd/zlib decoders.
It does not decode compressed 842. The static initrd build omits dynamic
libraries, but kernel attachment/supported compression still works.

## Safety and build

Use canonical absolute paths without symlink components and single-link regular
files. Backing paths may be user-owned or user-writable, including FAT/exFAT
media whose ownership and modes are synthesized by mount options. Writable
attachments hold exclusive cooperative flocks; readers use shared locks. Other
applications must not modify, rename or remove backing files while attached.
Users with write access to the backing directory can still corrupt or delete the
image, just like any other file; path permissions are not treated as a security
boundary. Copy the complete set only after detaching. There is no protection
against writers ignoring locks.
Buffered lower filesystems admitted by the driver are ext2/ext4, vfat, exfat,
ntfs3 and Btrfs, subject to geometry/attribute checks in `dynblk.c`. Direct modes
are currently limited to ext2/ext4; do not assume all admitted filesystems have
been exercised in the latest VM smoke test.

```sh
make
make dynblk-initrd
make -C /lib/modules/6.12.x/build M="$PWD" W=1 KCFLAGS=-Werror modules
make test
make install DESTDIR=/tmp/dynblk-stage
```

The module includes `dynblk_engine.c`; the checker compiles the same engine
against userspace adapters. `make test` uses private regular files, sanitizers,
a small synchronization-failure model and QEMU interoperability checks. It
never loads the module or formats host disks. The optional disposable kernel
smoke test is described in [tests/vm/README.md](tests/vm/README.md).
Debian packaging produces `dynblk` and `dynblk-dkms`. Installation does not
attach storage. Rebuild the static i686/musl payload separately for MiniOS;
see the parent repository's `linux-live/initramfs/buildroot/README.md`.
