# DynBlk storage formats and ABI

This document describes the engine in `dynblk_engine.c`, used by the Linux
module and offline checker. Native disk format is **1**, magic `DBSPRS01`.
Use a matching CLI/module pair; ioctl commands encode structure sizes.

## Common geometry

Metadata pages and native logical sectors are 4096 bytes. Compression grains
are 65536 bytes. VMDK logical sectors are 512 bytes. Create/grow sizes are
positive multiples of 4096. Requests are bounded to 256 KiB by the kernel
frontend. All on-disk integers are little-endian. Capacity is determined by geometry and resource limits.
A 65536-extent resource guard and a 4000-MiB physical cap per extent remain.
Native capacity is at most `span * 65536` (64 TiB with standard 1-GiB spans).
VMDK has a 128-TiB addressing ceiling, further restricted by descriptor size
and actual extent geometry. Extent/file arrays grow dynamically with the image;
no maximum-sized arrays are allocated on each attachment or on the kernel stack.
The VMDK text descriptor occupies a separate file slot. Headers and mapping entries are independent of the number of parts.

The engine has a per-device metadata cache of 1 MiB by default (1..64 MiB),
plus small resident directories and bounded working buffers. Cache pages cover
both mapping tables and native allocation bitmaps. Dirty eviction uses the
same ordered flush protocol as explicit flush. This is not a resident copy of
all live mappings. `de_memory()` counts owned engine buffers, excluding the
adapters, block layer, codec provider and host page cache. Attachment temporarily
uses one per-extent bitmap to check aliases and rebuild allocation state.

## Native extent header

Two 4096-byte copies, at file offsets 0 and 4096. Each has CRC32/IEEE over bytes
0..4091, stored at offset 4092. Select the valid copy with the higher generation;
read-only open never repairs either copy. These are geometry headers, not COW
roots and not a per-write transaction log.

| Offset | Bytes | Value |
|---|---|---|
| 0 | 8 | `DBSPRS01` |
| 8 | 4 | format = 1 |
| 12 | 4 | zero-based part index |
| 16 | 16 | native volume UUID |
| 32 | 8 | virtual capacity (part 0 is authoritative after grow) |
| 40 | 8 | fixed logical span per part |
| 48 | 8 | physical part limit |
| 56 | 4 | grain bytes = 65536 |
| 60 | 4 | mapping entry bytes = 16 |
| 64 | 16 | NUL-terminated codec name |
| 80 | 8 | allocation bitmap byte offset |
| 88 | 8 | allocation bitmap byte length |
| 96 | 8 | grain directory byte offset |
| 104 | 8 | grain tables byte offset |
| 112 | 8 | first possible payload byte offset |
| 120 | 8 | header generation |
| 128 | 3964 | zero-reserved |
| 4092 | 4 | header CRC32 |

The generation counter advances on engine flush; headers persist it on creation
and growth, not on every data write. It is not a persistent write counter.
All files share UUID, span, limit, codec and layout. Part 0's capacity is authoritative.

## Native layout and mapping

A part describes `span` bytes beginning at `part * span`. Creation chooses a
power-of-two span between 64 KiB and 1 GiB, no greater than half the physical
part cap. Normally it is 1 GiB. All declared parts have metadata initialized at
create/grow; unused data ranges remain sparse. There is no cross-part payload
sharing. With primary `volume000.db`, siblings are `volume%03u.db`; otherwise
siblings are `PRIMARY.%03u` (primary unchanged for part zero).

Bitmap begins at 8192: one bit per physical 4-KiB page, least-significant bit
first, padded to 4096 bytes. Grain directory follows; it occupies one 4-KiB
page containing 32-bit sector addresses of tables. Tables follow consecutively.
Each table is 4096 bytes with 256 mappings, covering 16 MiB of logical space.
Payload follows the last table. All metadata pages are reserved in the bitmap.
`de_native_geometry()` computes these offsets and rejects inconsistent headers.

Each 16-byte mapping is `{ offset:u64, length:u32, stored_crc:u32 }`.
All-zero is a hole. Offset is part-relative and 4096-aligned. Length 65536 means
raw, with stored_crc zero. A shorter nonzero length means the entire 64-KiB
grain was compressed using the header codec; stored_crc covers exactly that
many encoded bytes. Physical allocation rounds length up to 4096. The writer
uses compression only when the rounded allocation is smaller than 64 KiB.
No uncompressed-data checksum is stored. References into metadata, beyond file
length/cap, or overlapping another live payload are rejected during scanning.

Existing raw grains permit partial overwrite in place. A compressed partial
write decodes, updates and recompresses the whole grain, then changes one mapping.
All-zero complete native grains become holes. Partial raw zeroing need not.

## Flush, recovery and free space

Ordinary writeback writes may complete before durable publication. Flush:
1. Synchronize changed payload files (except explicit unsafe mode).
2. Write dirty mapping pages, then synchronize the affected files.
3. Retire superseded runs, update native bitmap bits and allocation hints;
   punch unreferenced ext4 payload ranges only after both mapping copies are durable.
4. Write dirty bitmap pages; trim requested completely free file tails.
   Unsafe mode never physically punches/truncates retired ranges.

Retired runs are kept until the mapping barrier. At most 256 retired runs are
queued; reaching that bound or evicting dirty metadata forces a flush. The
native allocator uses a wrapping first-fit bitmap search within the part. VMDK
uses unreferenced grain locations before extending a part. Allocation does not
move live data, even when contiguous free space is exhausted. Only explicit
RECLAIM with COMPACT may relocate payload into a non-overlapping earlier run.
Mappings are authoritative. Writable native open rebuilds allocation bitmaps
from mappings, so unsynchronized bitmap writes cannot authorize reuse on reopen.
Both formats scan metadata/bounds/overlaps at attachment. Only explicit full
`check` reads and decodes every payload. Read-only open does not repair anything.

This format deliberately does not implement the old dual-root COW transaction
protocol or a journal. Mapping writes are in-place; there is no guarantee of
multi-sector atomicity under torn writes or recovery from arbitrary damage.
Raw data has ordinary in-place overwrite semantics. The small crash-model test
covers modeled failed operations and completed synchronization, not every
possible storage-controller reorder, torn sector or hardware fault.

Kernel frontend handles FLUSH/PREFLUSH/FUA and forces completion synchronization
for writethrough/directsync. Deferred modes also schedule a bounded background
flush. Direct modes actually open files O_DIRECT (currently ext2/ext4 only);
unsafe omits durability barriers. These policies are not native on-disk fields.

## Supported VMDK profile

Primary: descriptor version 1, `createType="twoGbMaxExtentSparse"`,
`parentCID=ffffffff`, hexadecimal CID of one through eight digits and one or
more `RW SECTORS SPARSE "FILENAME"` lines. Only simple sibling filenames are
accepted: no path traversal, symlinks, parent images or duplicate file names.
`ddb.*` fields are tolerated. Descriptor size must be below 1 MiB, matching
the descriptor-reader bound in the reviewed QEMU implementation. Its buffer
and parsing scratch grow with the file rather than using a 64-KiB payload
buffer. Create/grow checks the required descriptor space before creating files.
Descriptors are read up to the first NUL, matching QEMU: a shortened CID can
leave a stale suffix in the rest of the sector. Bytes beyond that terminator
are ignored and are never parsed as additional directives. Writable open changes CID, preserving surrounding fields and expanding
short CIDs safely. Read-only open leaves descriptor bytes/timestamps unchanged.

Each extent has little-endian magic 0x564d444b (`KDMV` on disk), sparse version
1 or supported zero-grain version 2, 128-sector grains and 512-entry tables.
Only flag bits 0..2 are accepted; compression/markers/SESparse are unsupported.
Directory entries and GTEs are 32-bit sector offsets. GTE zero means absent;
GTE one means zero only when the zero-grain flag is set. Allocated grains are
64-KiB aligned. Table, directory and payload ranges are checked before use.

Creation writes standard primary/redundant directories and tables, with extents
covering up to 2 GiB each. Payload grows as needed. Existing raw grains are
written in place; new grains extend the file and change primary/backup GTEs.
Partial first writes zero-initialize the other bytes. Growth appends extents,
even when an earlier last extent is smaller than 2 GiB. Descriptor replacement
on growth is not a transactional snapshot operation. Full-grain discard clears
primary/redundant GTEs (only parentless images are admitted). After the mapping
barrier the old grain can be reused, hole-punched or removed by tail truncation.
Partial discard still zeroes data without necessarily releasing the whole grain. No DynBlk sidecar metadata is required to open a
VMDK with another implementation after detachment.

## Device ABI and lifetime

`dynblk_uapi.h` is authoritative for native-endian ioctl structures and command
numbers. ABI version is 1. GET and ATTACH validate their size-encoded ioctl
numbers and require a matching CLI/module pair. Format selectors:
auto=0, native=1, VMDK=2. Cache selectors: writeback=0, writethrough=1, none=2,
directsync=3, unsafe=4. Options belong to each attachment, not to stored policy.

`/dev/dynblk-control` is root-only. ATTACH creates a whole disk and returns its
index and opaque cookie. GROW, GET, COOKIE and AUTOCLEAR are whole-disk ioctls.
AUTOCLEAR must be armed while holding the matching whole-disk descriptor.
DETACH checks index/cookie and refuses live opens. Last-close autoclear runs
outside block release locks; retained partition/bind/lazy-mount references keep
the device alive. Manual loads remain until explicit detach. The module cannot
be unloaded while attachments or teardown work still hold it.

GET reports format, cache policy, allocated-grain bytes (rounded and capped by
capacity), encoded bytes, file lengths, cache statistics and accounted buffers.
`physical_bytes` is summed file lengths, not filesystem allocated sectors.
VMDK's reported UUID is attachment-local, not a portable on-disk identity.
Both formats advertise reclamation; only native advertises compression.

Mutations are serialized per device in reclaim-safe NOIO context. No NOWAIT,
polling, atomic-write contract, shrink, live cache-mode switching or automatic
filesystem resizing is provided. Host path/attribute checks and locking are
shared by both formats; offline checking never changes storage.

## Reclaim operation (no disk-layout change)

RECLAIM is a cookie-protected whole-disk ioctl. One call advances a bounded
cursor: normally up to 1024 grains or 4 MiB moved, or 64 MiB of hole scanning.
The initial evictable physical-usage bitmap may require scanning one whole
part's tables. The bitmap tracks pending retirements and ordinary writes;
all steps use the device state lock. The CLI retains its fd between steps.
A cursor is a best-effort pass, not a snapshot of concurrently changing data.

Default flags forbid relocation. Without a punching callback, or after its
EOPNOTSUPP, the only physical action is truncating completely free tails.
COMPACT is explicit and copies the encoded payload, reserves its destination,
flushes destination data before primary/backup map publication, and only then
retires the source. No background path passes COMPACT. ZEROES is an explicit
additional read scan, never an assumption that deleted files contain zeroes.
Unsafe mode rejects reclaim; read-only rejects all modifying operations.
No journal, all-or-nothing multi-grain transaction or hardware atomicity beyond
the rest of this format is claimed by this command.
