"""Self-contained dynblk acceptance; executable only in the disposable Testo VM."""
import contextlib
import errno
import fcntl
import hashlib
import json
import mmap
import os
from pathlib import Path
import signal
import stat
import struct
import subprocess
import time
import traceback
import zlib

B = 4096
C = 65536
MIB = 1024 ** 2
ROOT = Path('/back')
DEVICE = '/dev/dynblk0'
CLI = '/usr/sbin/dynblk'
RESULTS = {}
IO_METRICS = {'writes': 0, 'max_write_seconds': 0.0}


def dynblk_ioctl(direction, size, number):
    return (direction << 30) | (size << 16) | (0xD7 << 8) | number


DYNBLK_ATTACH_IOCTL = dynblk_ioctl(3, 4152, 2)
DYNBLK_DETACH_IOCTL = dynblk_ioctl(1, 16, 3)
DYNBLK_COOKIE_IOCTL = dynblk_ioctl(2, 8, 4)


def safety():
    tokens = Path('/proc/cmdline').read_text().split()
    assert 'dynblk_disposable_acceptance=1' in tokens
    assert 'dynblk_selfcontained=1' in tokens
    assert os.geteuid() == 0 and os.uname().release.startswith('6.12.')
    assert Path('/sys/class/block/sda/size').read_text().strip() == '16777216'
    assert not Path('/dev/sdb').exists()
    assert command('blkid', '-s', 'LABEL', '-o', 'value', '/dev/sda').strip() == 'DYNBLK-SELF'
    assert command('findmnt', '-n', '-o', 'SOURCE', '--target', ROOT).strip() == '/dev/sda'


def command(*args, success=True):
    print('RUN', *args, flush=True)
    result = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=180)
    print(result.stdout, end='', flush=True)
    print(result.stderr, end='', flush=True)
    assert (result.returncode == 0) == success, (args, result.returncode)
    return result.stdout


def cli(*args, success=True):
    return command(CLI, *args, success=success)


def durable(path, value):
    temporary = path.with_name(path.name + '.new')
    with temporary.open('w') as stream:
        stream.write(json.dumps(value, indent=2))
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)
    fd = os.open(path.parent, os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def record(name, value=True):
    RESULTS[name] = value
    durable(ROOT / 'results.json', RESULTS)
    outcome = value.get('outcome', 'PASS') if isinstance(value, dict) else 'PASS'
    print(outcome, name, value, flush=True)


def kernel_log(name):
    with (ROOT / name).open('w') as stream:
        subprocess.run(['dmesg'], stdout=stream, check=True)
        os.fsync(stream.fileno())
    fd = os.open(ROOT, os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def inventory(volume):
    entries = {}
    for path in sorted(volume.iterdir()):
        info = path.lstat()
        assert stat.S_ISREG(info.st_mode), path
        assert path.name.startswith('volume') and path.suffix == '.db', path
        entries[path.name] = {'bytes': info.st_size, 'allocated': info.st_blocks * 512}
    return entries


def file_bytes(volume):
    return sum(entry['bytes'] for entry in inventory(volume).values())


def hashes(volume):
    assert not Path('/sys/module/dynblk').exists(), 'detach before reading backing files'
    result = {}
    for name in inventory(volume):
        with (volume / name).open('rb') as stream:
            result[name] = hashlib.file_digest(stream, 'sha256').hexdigest()
    return result


def io_deadline(_signum, _frame):
    raise TimeoutError('dynblk device I/O exceeded 120 seconds')


def bounded_steps(count, label):
    started = time.monotonic()
    for index in range(count):
        yield index
        elapsed = time.monotonic() - started
        assert elapsed < 300, ('workload batch exceeded five minutes', label, index, elapsed)
        if (index + 1) % 128 == 0 or index + 1 == count:
            print('PROGRESS', label, index + 1, '/', count, 'batch_seconds', round(elapsed, 3), flush=True)
            started = time.monotonic()


def direct(offset, data=None, length=B):
    started = time.monotonic()
    fd = os.open(DEVICE, os.O_RDWR | os.O_DIRECT | os.O_DSYNC)
    buffer = mmap.mmap(-1, length if data is None else len(data))
    signal.setitimer(signal.ITIMER_REAL, 120)
    try:
        if data is None:
            assert os.preadv(fd, [buffer], offset) == length
            return buffer[:]
        buffer[:] = data
        assert os.pwrite(fd, buffer, offset) == len(data)
        os.fsync(fd)
        elapsed = time.monotonic() - started
        IO_METRICS['writes'] += 1
        IO_METRICS['max_write_seconds'] = max(IO_METRICS['max_write_seconds'], elapsed)
        assert elapsed < 120, ('individual write latency exceeded 120s', elapsed, offset)
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        buffer.close()
        os.close(fd)


def discard(offset, length):
    queue = Path('/sys/class/block/dynblk0/queue')
    print('DISCARD', {'offset': offset, 'length': length,
          'max_bytes': int((queue / 'discard_max_bytes').read_text()),
          'granularity': int((queue / 'discard_granularity').read_text())}, flush=True)
    with open(DEVICE, 'rb+', buffering=0) as stream:
        fcntl.ioctl(stream, 0x1277, struct.pack('=QQ', offset, length))
        os.fsync(stream.fileno())
    assert direct(offset, length=length) == bytes(length)


def status(allow_fenced=False):
    raw = bytearray(128)
    with open(DEVICE, 'rb', buffering=0) as stream:
        fcntl.ioctl(stream, 0x8080D700, raw)
    fields = ('version', 'flags', 'uuid', 'algorithm', 'capacity', 'original',
              'stored', 'physical', 'generation', 'part_limit', 'parts',
              'block_size', 'max_parts', 'tree_height', 'map_memory',
              'max_capacity', 'compression_region_bytes')
    value = dict(zip(fields, struct.unpack('=II16s16sQQQQQQIIIIQQQ', raw)))
    assert value['version'] == 1 and value['flags'] in ((0, 1) if allow_fenced else (0,)), value
    assert value['block_size'] == B and value['max_parts'] == 64
    assert value['tree_height'] == 4 and value['compression_region_bytes'] == C
    assert value['max_capacity'] == 512 * 1024 * MIB
    value['uuid'] = value['uuid'].hex()
    value['algorithm'] = value['algorithm'].split(b'\0', 1)[0].decode('ascii')
    assert Path('/dev/dynblk-control').is_char_device()
    return value


def grow(capacity):
    with open(DEVICE, 'rb+', buffering=0) as stream:
        fcntl.ioctl(stream, 0x4008D701, struct.pack('=Q', capacity))
        os.fsync(stream.fileno())
    assert status()['capacity'] == capacity
    assert int(command('blockdev', '--getsize64', DEVICE)) == capacity


def load(volume):
    attached = cli('load', volume / 'volume000.db', '--module', '/cli/dynblk.ko', '--execute').strip()
    assert attached == DEVICE
    assert stat.S_ISBLK(os.stat(DEVICE).st_mode)
    return status()


def unload():
    cli('unload', DEVICE, '--execute')
    assert Path('/sys/module/dynblk').exists() and Path('/dev/dynblk-control').is_char_device()
    assert not Path(DEVICE).exists()
    command('rmmod', 'dynblk')
    assert not Path('/sys/module/dynblk').exists() and not Path('/dev/dynblk-control').exists()


def attachment_cookie(device=DEVICE):
    raw = bytearray(8)
    fd = os.open(device, os.O_RDONLY | os.O_NONBLOCK)
    try:
        fcntl.ioctl(fd, DYNBLK_COOKIE_IOCTL, raw, True)
    finally:
        os.close(fd)
    cookie = struct.unpack('=Q', raw)[0]
    assert cookie
    return cookie


def control_abi_guards():
    base = ROOT / 'control-abi'
    first, second = base / 'first', base / 'second'
    first.mkdir(parents=True, mode=0o700)
    second.mkdir(mode=0o700)
    assert cli('create', first / 'volume000.db', '--module', '/cli/dynblk.ko', '--execute').strip() == DEVICE
    stale = attachment_cookie()
    cli('unload', DEVICE, '--execute')
    assert cli('create', second / 'volume000.db', '--module', '/cli/dynblk.ko', '--execute').strip() == DEVICE
    current = attachment_cookie()
    assert current != stale
    request = struct.pack('=IIQ', 1, 0, stale)
    fd = os.open('/dev/dynblk-control', os.O_RDWR)
    try:
        try:
            fcntl.ioctl(fd, DYNBLK_DETACH_IOCTL, request)
        except OSError as exc:
            assert exc.errno == errno.ESTALE, exc
        else:
            raise AssertionError('stale attachment cookie detached a replacement device')
    finally:
        os.close(fd)
    assert Path(DEVICE).is_block_device()
    cli('unload', DEVICE, '--execute')
    command('rmmod', 'dynblk')
    record('attachment_cookie_reuse_guard', {'stale': stale, 'current': current})


def kernel_namespace_guard():
    parent = ROOT / 'kernel-namespace'
    parent.mkdir(mode=0o700)
    sibling = parent / 'volume001.db'
    sibling.write_bytes(b'keep')
    sibling.chmod(0o600)
    command('insmod', '/cli/dynblk.ko')
    request = bytearray(4152)
    struct.pack_into('=IIiIQQQ16s', request, 0, 1, 1, -1, 256,
                     64 * MIB, 2 * MIB, 0, b'none\0')
    path = str(parent / 'volume000.db').encode()
    request[56:56 + len(path) + 1] = path + b'\0'
    fd = os.open('/dev/dynblk-control', os.O_RDWR)
    try:
        try:
            fcntl.ioctl(fd, DYNBLK_ATTACH_IOCTL, request, True)
        except OSError as exc:
            assert exc.errno == errno.EEXIST, exc
        else:
            raise AssertionError('kernel accepted create with occupied sibling namespace')
    finally:
        os.close(fd)
    assert not (parent / 'volume000.db').exists()
    assert sibling.read_bytes() == b'keep'
    command('rmmod', 'dynblk')
    record('kernel_unused_namespace_guard')


def fenced_detach_guard(lower):
    volume = lower / 'fenced-detach'
    create(volume, part_limit=MIB)
    acknowledged = 0
    failure = None
    for index in bounded_steps(1024, 'audit/fenced-fill'):
        data = hashlib.shake_256(b'audit-fenced-fill' + struct.pack('<Q', index)).digest(2 * C)
        try:
            direct(index * 2 * C, data)
        except OSError as exc:
            assert exc.errno in (errno.ENOSPC, errno.EIO), exc
            failure = exc.errno
            break
        acknowledged = index + 1
    assert failure is not None and acknowledged > 1, (failure, acknowledged)
    fenced = status(allow_fenced=True)
    assert fenced['flags'] == 1
    first = hashlib.shake_256(b'audit-fenced-fill' + struct.pack('<Q', 0)).digest(2 * C)
    last_index = acknowledged - 1
    last = hashlib.shake_256(b'audit-fenced-fill' + struct.pack('<Q', last_index)).digest(2 * C)
    assert direct(0, length=2 * C) == first
    assert direct(last_index * 2 * C, length=2 * C) == last
    unload()
    load(volume)
    assert direct(0, length=2 * C) == first
    assert direct(last_index * 2 * C, length=2 * C) == last
    unload()
    record('fenced_detach_recovery_guard', {'acknowledged_batches': acknowledged, 'errno': failure})


def tree_index_budget_guard():
    base = ROOT / 'tree-index-budget'
    first, second, third = base / 'first', base / 'second', base / 'third'
    first.mkdir(parents=True, mode=0o700)
    second.mkdir(mode=0o700)
    third.mkdir(mode=0o700)
    # The fixed index is now only 396,312 bytes/device: a 1 MiB global budget
    # admits two devices and rejects the third before numeric slots are exhausted.
    command('insmod', '/cli/dynblk.ko', 'tree_index_budget_mb=1')
    assert cli('create', first / 'volume000.db', '--size', '64MiB', '--execute').strip() == DEVICE
    assert cli('create', second / 'volume000.db', '--size', '64MiB', '--execute').strip() == '/dev/dynblk1'
    result = subprocess.run([CLI, 'create', str(third / 'volume000.db'), '--size', '64MiB', '--execute'],
                            capture_output=True, text=True)
    assert result.returncode and 'Cannot allocate memory' in result.stderr, result
    assert not list(third.iterdir())
    cli('unload', '/dev/dynblk1', '--execute')
    cli('unload', DEVICE, '--execute')
    command('rmmod', 'dynblk')
    record('tree_index_budget_guard', {'budget_mb': 1, 'admitted_devices': 2,
                                       'third_attach_errno': errno.ENOMEM})


def mapping_memory_guard():
    volume = ROOT / 'mapping-memory' / 'volume'
    volume.parent.mkdir(mode=0o700)
    create(volume)
    fixed = status()['map_memory']
    assert fixed == 16513 * 24, fixed
    payload = hashlib.shake_256(b'dynblk-mapping-memory').digest(32 * MIB)
    direct(0, payload)
    dense = status()['map_memory']
    # 32 MiB dense logical data = 64 runtime chunks = 256 KiB plus fixed index.
    assert dense == fixed + 64 * B, (fixed, dense)
    discard(0, 32 * MIB)
    reclaimed = status()['map_memory']
    assert reclaimed == fixed, (fixed, reclaimed)
    unload()
    record('mapping_memory_density', {'fixed_bytes': fixed,
                                      'dense_32mib_bytes': dense,
                                      'dense_bytes_per_gib': 8 * MIB,
                                      'reclaimed_bytes': reclaimed})


def create(volume, algorithm=None, part_limit=None):
    volume.mkdir(mode=0o700)
    args = ['create', volume / 'volume000.db', '--module', '/cli/dynblk.ko']
    if algorithm is not None:
        args += ['--compression', algorithm]
    if part_limit is not None:
        args += ['--part-size', part_limit]
    cli(*args)
    assert not list(volume.iterdir()), 'dry-run creation wrote storage'
    cli(*args, '--execute')
    value = status()
    actual = inventory(volume)
    if volume.parent.name == 'lower-btrfs':
        with (volume / 'volume000.db').open('rb') as stream:
            flags = bytearray(8)
            fcntl.ioctl(stream, 0x80086601, flags)
        assert not struct.unpack_from('=I', flags)[0] & 0x00800000, 'Btrfs NOCOW is not permitted'
    assert set(actual) == {'volume000.db'} and actual['volume000.db']['bytes'] == 3 * B
    assert value['capacity'] == 16 * 1024 * MIB and value['parts'] == 1
    assert value['algorithm'] == (algorithm or 'none')
    assert value['original'] == value['stored'] == 0
    assert value['physical'] == 3 * B
    assert value['part_limit'] == (part_limit or 4000 * MIB)
    reported = json.loads(cli('status', DEVICE, '--json'))
    assert reported['capacity_bytes'] == value['capacity'] and reported['compression'] == value['algorithm']
    assert direct(0, length=128 * 1024) == bytes(128 * 1024)
    assert direct(value['capacity'] - B) == bytes(B)
    record('initial_' + volume.parent.name + '_' + volume.name, {'files': actual, 'status': value})
    unload()
    before = hashes(volume)
    report = json.loads(cli('inspect', volume / 'volume000.db', '--metadata-only', '--json'))
    assert report['fully_validated'] is False
    assert hashes(volume) == before
    load(volume)
    assert inventory(volume) == actual


def functional(lower, codec, raw_size):
    volume = lower / ('functional-' + codec)
    create(volume, None if codec == 'none' else codec)
    # Aligned 64KiB clusters, with 16 distinguishable logical sectors in each.
    compressible = b''.join(bytes([index % 251 + 1]) * B for index in range(16 * 16))
    random = hashlib.shake_256(b'dynblk-incompressible-fixture').digest(16 * C)
    payload = compressible + random
    for offset in range(0, len(payload), 32 * B):
        direct(offset, payload[offset:offset + 32 * B])
    before = status()
    assert before['original'] == len(payload)
    assert before['physical'] == file_bytes(volume)
    if codec == 'none':
        assert before['stored'] == len(payload)
        raw_size = before['physical']
    else:
        assert before['stored'] < len(payload) * 3 // 4
        assert before['physical'] < raw_size - len(compressible) // 2, (codec, before, raw_size)
    unload()
    mappings = checkpoint(volume)
    assert set(mappings) == set(range(len(payload) // B))
    assert all(mappings[i]['flags'] == 0 and mappings[i]['length'] == B for i in range(256, 512))
    if codec != 'none':
        for base in range(0, 256, 16):
            assert len({mappings[i]['pointer'] for i in range(base, base + 16)}) == 1
            assert all(mappings[i]['flags'] == 1 and mappings[i]['decoded_length'] == C
                       and mappings[i]['decoded_offset'] == (i - base) * B for i in range(base, base + 16))
    load(volume)
    assert hashlib.sha256(direct(0, length=len(payload))).digest() == hashlib.sha256(payload).digest()
    # Partial-cluster COW and discard must retain all 15 neighbouring sectors.
    replacement = hashlib.shake_256(b'partial-cluster').digest(B)
    direct(7 * B, replacement)
    partial = payload[:7 * B] + replacement + payload[8 * B:C]
    assert direct(0, length=C) == partial
    unload()
    partial_map = checkpoint(volume)
    assert partial_map[7]['decoded_length'] == B and partial_map[7]['decoded_offset'] == 0
    assert all(partial_map[index] == mappings[index] for index in range(16) if index != 7)
    load(volume)
    discard(7 * B, B)
    assert direct(0, length=C) == payload[:7 * B] + bytes(B) + payload[8 * B:C]
    unload()
    discarded_map = checkpoint(volume)
    assert 7 not in discarded_map
    assert all(discarded_map[index] == mappings[index] for index in range(16) if index != 7)
    load(volume)
    direct(7 * B, payload[7 * B:8 * B])
    # Many COW/checkpoint generations over a fixed live set must not append forever.
    sizes = []
    for turn in bounded_steps(512, lower.name + '/' + codec + '/cow'):
        index = turn % 32
        value = hashlib.shake_256(struct.pack('<Q', turn)).digest(B)
        direct(index * B, value)
        payload = payload[:index * B] + value + payload[(index + 1) * B:]
        if turn % 32 == 31:
            sizes.append(file_bytes(volume))
    assert max(sizes[4:]) <= max(sizes[:4]) + 4 * C, sizes
    assert sizes[-1] < before['physical'] + 2 * MIB, sizes
    identity = status()['uuid']
    grow(17 * 1024 * MIB)
    cli('grow', DEVICE, 18 * 1024 * MIB, '--execute')
    assert status()['capacity'] == 18 * 1024 * MIB
    generation = status()['generation']
    grow(18 * 1024 * MIB)
    assert status()['generation'] == generation
    assert direct(17 * 1024 * MIB - B) == bytes(B)
    discard(64 * B, 32 * B)
    payload = payload[:64 * B] + bytes(32 * B) + payload[96 * B:]
    expected = hashlib.sha256(payload).hexdigest()
    unload()
    load(volume)
    assert status()['uuid'] == identity and status()['capacity'] == 18 * 1024 * MIB
    assert hashlib.sha256(direct(0, length=len(payload))).hexdigest() == expected
    unload()
    record('functional_' + lower.name + '_' + codec,
           {'payload_sha256': expected, 'physical_before': before['physical'],
            'raw_physical': raw_size, 'stored': before['stored'], 'cow_sizes': sizes,
            'cow_iterations': 512, 'reload': True, 'discard_zero': True})
    return raw_size


def corrupted_committed_payload(lower):
    volume = lower / 'corrupted-payload'
    create(volume)
    direct(0, b'C' * B)
    unload()
    mapping = checkpoint(volume)[0]
    pointer = mapping['pointer']
    path = volume / ('volume%03d.db' % (pointer >> 32))
    with path.open('rb+') as stream:
        stream.seek(pointer & 0xFFFFFFFF)
        byte = stream.read(1)
        stream.seek(pointer & 0xFFFFFFFF)
        stream.write(bytes([byte[0] ^ 1]))
        stream.flush()
        os.fsync(stream.fileno())
    before = hashes(volume)
    cli('load', volume / 'volume000.db', '--module', '/cli/dynblk.ko', '--execute', success=False)
    assert not Path('/sys/module/dynblk').exists() and not Path(DEVICE).exists()
    assert hashes(volume) == before, 'rejected committed corruption changed backing files'
    record('committed_crc_rejected_' + lower.name)


def missing_committed_part(volume):
    mappings = checkpoint(volume)
    committed = {entry['pointer'] >> 32 for entry in mappings.values()} - {0}
    assert committed, 'fixture must commit payload to a nonzero part before deletion'
    part = min(committed)
    path = volume / ('volume%03d.db' % part)
    saved = volume.parent / (volume.name + '-removed-part')
    path.rename(saved)
    before = hashes(volume)
    try:
        cli('inspect', volume / 'volume000.db', '--metadata-only', '--json', success=False)
        cli('load', volume / 'volume000.db', '--module', '/cli/dynblk.ko', '--execute', success=False)
        assert not Path('/sys/module/dynblk').exists() and not Path(DEVICE).exists()
        assert hashes(volume) == before and not path.exists(), 'missing committed part was recreated'
    finally:
        assert not Path('/sys/module/dynblk').exists()
        saved.rename(path)
    load(volume)
    unload()
    record('missing_committed_part_' + volume.parent.name, {'part': part})


def lazy_gc(lower):
    volume = lower / 'lazy-gc'
    create(volume, part_limit=2 * MIB)
    data = hashlib.shake_256(b'lazy-gc-16MiB').digest(16 * MIB)
    for offset in range(0, len(data), 2 * C):
        direct(offset, data[offset:offset + 2 * C])
    initial = inventory(volume)
    assert len(initial) >= 3 and all(entry['bytes'] <= 2 * MIB for entry in initial.values())
    unload()
    incarnations = part_incarnations(volume)
    missing_committed_part(volume)
    load(volume)
    assert status()['part_limit'] == 2 * MIB, 'small part limit did not persist'
    assert hashlib.sha256(direct(0, length=len(data))).digest() == hashlib.sha256(data).digest()
    discard(4 * MIB, 12 * MIB)
    expected = bytearray(data[:4 * MIB])
    sizes = []
    for turn in bounded_steps(1024, lower.name + '/lazy-gc'):
        offset = (turn % (len(expected) // B)) * B
        value = hashlib.shake_256(struct.pack('<Q', turn) + b'gc').digest(B)
        direct(offset, value)
        expected[offset:offset + B] = value
        if turn % 64 == 63:
            sizes.append(file_bytes(volume))
    assert max(sizes[4:]) <= max(sizes[:4]) + 2 * MIB, sizes
    assert sizes[-1] < 12 * MIB and len(inventory(volume)) < len(initial), (initial, inventory(volume), sizes)
    assert direct(0, length=len(expected)) == expected
    assert direct(4 * MIB, length=12 * MIB) == bytes(12 * MIB)
    unload()
    after_gc = part_incarnations(volume)
    retired = set(incarnations) - set(after_gc)
    assert retired, 'GC did not retire any part'
    load(volume)
    for offset in range(4 * MIB, len(data), 2 * C):
        direct(offset, data[offset:offset + 2 * C])
    unload()
    recreated = part_incarnations(volume)
    reused = retired & set(recreated)
    assert reused and all(recreated[name] != incarnations[name] for name in reused)
    load(volume)
    expected.extend(data[4 * MIB:])
    digest = hashlib.sha256(direct(0, length=len(expected))).hexdigest()
    assert digest == hashlib.sha256(expected).hexdigest()
    unload()
    record('lazy_gc_' + lower.name, {'initial': initial, 'sizes': sizes,
           'retired': sorted(retired), 'recreated': sorted(reused), 'payload_sha256': digest})


def part_incarnations(volume):
    assert not Path('/sys/module/dynblk').exists()
    result = {}
    for name in inventory(volume):
        with (volume / name).open('rb') as stream:
            data = stream.read(B)
        assert data[:8] == b'DBPART01'
        assert zlib.crc32(data[:4092]) == struct.unpack_from('<I', data, 4092)[0]
        result[name] = struct.unpack_from('<Q', data, 24)[0]
    return result


def corrupted_tree(lower):
    volume = lower / 'corrupted-tree'
    create(volume)
    direct(0, b'T' * C)
    unload()
    metadata = {}
    checkpoint(volume, metadata)
    leaf = next(pointer for pointer, data in metadata.items()
                if data[:8] == b'DBTREE01' and struct.unpack_from('<I', data, 40)[0] == 0)
    path = volume / ('volume%03d.db' % (leaf >> 32))
    with path.open('rb+') as stream:
        offset = (leaf & 0xFFFFFFFF) + 4000
        stream.seek(offset)
        old = stream.read(1)
        stream.seek(offset)
        stream.write(bytes([old[0] ^ 1]))
        stream.flush()
        os.fsync(stream.fileno())
    before = hashes(volume)
    cli('load', volume / 'volume000.db', '--module', '/cli/dynblk.ko', '--execute', success=False)
    assert not Path('/sys/module/dynblk').exists() and not Path(DEVICE).exists()
    assert hashes(volume) == before
    record('committed_tree_crc_rejected_' + lower.name)


def incremental_metadata(lower):
    volume = lower / 'incremental-map'
    create(volume)
    clusters = 8192
    for index in bounded_steps(clusters, 'metadata/write'):
        direct(index * C, bytes([index % 251 + 1]) * C)
    assert status()['original'] == clusters * C
    unload()
    old_pages = {}
    old_map = checkpoint(volume, old_pages)
    assert len(old_map) == clusters * 16 and len(old_pages) * B > 128 * 1024
    load(volume)
    index = clusters // 2
    replacement = hashlib.shake_256(b'incremental-one-sector').digest(B)
    physical_before = file_bytes(volume)
    direct(index * C + 7 * B, replacement)
    physical_after = file_bytes(volume)
    expected = bytes([index % 251 + 1]) * (7 * B) + replacement + bytes([index % 251 + 1]) * (8 * B)
    assert direct(index * C, length=C) == expected
    unload()
    new_pages = {}
    new_map = checkpoint(volume, new_pages)
    rewritten = sum(B for pointer, data in new_pages.items() if old_pages.get(pointer) != data)
    retained = sum(1 for pointer, data in new_pages.items() if old_pages.get(pointer) == data)
    assert rewritten < 128 * 1024, ('whole-map rewrite rather than incremental path', rewritten)
    assert retained >= len(old_pages) * 9 // 10
    assert physical_after - physical_before < 128 * 1024
    assert all(new_map[key] == value for key, value in old_map.items() if key != index * 16 + 7)
    load(volume)
    digest = hashlib.sha256()
    for cluster in bounded_steps(clusters, 'metadata/reload-read'):
        data = direct(cluster * C, length=C)
        assert data == (expected if cluster == index else bytes([cluster % 251 + 1]) * C)
        digest.update(data)
    unload()
    record('incremental_metadata', {'mapped_clusters': clusters, 'logical_payload_bytes': clusters * C,
           'old_metadata_bytes': len(old_pages) * B, 'rewritten_metadata_bytes': rewritten,
           'retained_metadata_pages': retained, 'file_growth': physical_after - physical_before,
           'payload_sha256': digest.hexdigest()})


def nearfull_pool(lower):
    volume = lower / 'nearfull-pool'
    create(volume, part_limit=MIB)
    expected = bytearray()
    for index in bounded_steps(400, 'pool/fill'):
        data = hashlib.shake_256(b'nearfull-batch' + struct.pack('<Q', index)).digest(2 * C)
        direct(len(expected), data)
        expected.extend(data)
    assert len(expected) == 50 * MIB
    highwater = []
    for turn in bounded_steps(1024, 'pool/nearfull-cow'):
        offset = ((turn * 17) % (len(expected) // B)) * B
        value = hashlib.shake_256(b'nearfull-cow' + struct.pack('<Q', turn)).digest(B)
        direct(offset, value)
        expected[offset:offset + B] = value
        if turn % 64 == 63:
            highwater.append(file_bytes(volume))
    assert max(highwater[4:]) <= max(highwater[:4]) + 8 * MIB, highwater
    failure = None
    for index in bounded_steps((14 * MIB) // B + 1, 'pool/exhaustion'):
        value = hashlib.shake_256(b'nearfull-tail' + struct.pack('<Q', index)).digest(B)
        try:
            direct(len(expected), value)
        except OSError as exc:
            assert exc.errno in (errno.ENOSPC, errno.EIO), exc
            failure = exc.errno
            break
        expected.extend(value)
    assert failure is not None, 'bounded 64MiB pool did not exhaust'
    fenced = status(allow_fenced=True)
    assert fenced['flags'] == 1 and fenced['part_limit'] == MIB
    before = hashlib.sha256(expected).hexdigest()
    assert hashlib.sha256(direct(0, length=len(expected))).hexdigest() == before
    unload()
    load(volume)
    assert hashlib.sha256(direct(0, length=len(expected))).hexdigest() == before
    acknowledged = len(expected)
    # Discard also removes any unacknowledged tail left by the failed write.
    discard(16 * MIB, 48 * MIB)
    del expected[16 * MIB:]
    reused_sizes = []
    for turn in bounded_steps(1024, 'pool/reuse'):
        offset = ((turn * 17) % (len(expected) // B)) * B
        value = hashlib.shake_256(b'nearfull-reuse' + struct.pack('<Q', turn)).digest(B)
        direct(offset, value)
        expected[offset:offset + B] = value
        if turn % 64 == 63:
            reused_sizes.append(file_bytes(volume))
    assert reused_sizes[-1] < 32 * MIB, reused_sizes
    assert max(reused_sizes[4:]) <= max(reused_sizes[:4]) + 4 * MIB, reused_sizes
    after = hashlib.sha256(expected).hexdigest()
    unload()
    load(volume)
    assert hashlib.sha256(direct(0, length=len(expected))).hexdigest() == after
    assert direct(16 * MIB, length=48 * MIB) == bytes(48 * MIB)
    unload()
    record('nearfull_pool', {'acknowledged_bytes_before_enospc': acknowledged, 'errno': failure,
           'part_cap': MIB, 'max_parts': 64, 'nearfull_cow_iterations': 1024,
           'reuse_cow_iterations': 1024, 'nearfull_sizes': highwater, 'reuse_sizes': reused_sizes,
           'acknowledged_sha256': before, 'reused_sha256': after, 'latency': dict(IO_METRICS)})


def functional_matrix():
    codecs = ('none', 'lz4', 'lz4hc', 'lzo', 'lzo-rle', 'zstd', 'deflate')
    for kind in ('ntfs3', 'ext4', 'vfat', 'exfat', 'ext2', 'btrfs'):
        with lower_filesystem(kind) as lower:
            if kind == 'ntfs3':
                ntfs_compressed_parent(lower)
            raw_size = None
            for codec in codecs:
                raw_size = functional(lower, codec, raw_size)
            providers = {line.split(':', 1)[1].strip() for line in Path('/proc/crypto').read_text().splitlines()
                         if line.startswith('name')}
            assert set(codecs[1:]) <= providers
            record('runtime_codecs_' + kind, {'tested': list(codecs),
                   'registered_crypto_names': sorted(set(codecs[1:]) & providers)})
            corrupted_committed_payload(lower)
            corrupted_tree(lower)
            lazy_gc(lower)
            if kind == 'ext4':
                nearfull_pool(lower)
                incremental_metadata(lower)
            if kind == 'btrfs':
                btrfs_nocow_parent(lower)
            unsupported = lower / 'codec-842'
            unsupported.mkdir(mode=0o700)
            cli('create', unsupported / 'volume000.db', '--compression', '842',
                '--module', '/cli/dynblk.ko', '--execute', success=False)
            assert not Path('/sys/module/dynblk').exists() and not Path(DEVICE).exists()
            assert not list(unsupported.iterdir()), 'unsupported codec created storage'
            record('codec_842_rejected_' + kind)


def ntfs_compressed_parent(lower):
    parent = lower / 'compressed-parent'
    parent.mkdir(mode=0o700)
    original = os.getxattr(parent, 'system.ntfs_attrib')
    assert len(original) == 4
    wanted = struct.unpack('<I', original)[0] | 0x0800
    try:
        os.setxattr(parent, 'system.ntfs_attrib', struct.pack('<I', wanted))
    except OSError as exc:
        if exc.errno not in (errno.EOPNOTSUPP, errno.EINVAL, errno.EPERM):
            raise
        record('ntfs3_compressed_parent', {'outcome': 'UNAVAILABLE',
               'reason': 'native attribute API refused compression on the empty directory', 'errno': exc.errno})
        return
    actual = os.getxattr(parent, 'system.ntfs_attrib')
    assert struct.unpack('<I', actual)[0] & 0x0800
    before_mode = stat.S_IMODE(parent.stat().st_mode)
    os.chmod(parent, 0o700)
    info = parent.stat()
    assert info.st_uid == 0 and stat.S_IMODE(info.st_mode) == 0o700
    assert os.getxattr(parent, 'system.ntfs_attrib') == actual
    print('NTFS COMPRESSED PARENT', {'attributes': actual.hex(),
          'mode_after_setxattr': oct(before_mode), 'private_mode': oct(stat.S_IMODE(info.st_mode))}, flush=True)
    result = subprocess.run([CLI, 'create', str(parent / 'volume000.db'), '--module',
        '/cli/dynblk.ko', '--execute'], capture_output=True, text=True)
    assert result.returncode and 'Operation not supported' in result.stderr, result
    assert not list(parent.iterdir()) and not Path('/sys/module/dynblk').exists()
    assert os.getxattr(parent, 'system.ntfs_attrib') == actual
    record('ntfs3_compressed_parent_rejected', {'attributes': actual.hex(), 'stderr': result.stderr})


def btrfs_nocow_parent(lower):
    parent = lower / 'nocow-parent'
    parent.mkdir(mode=0o700)
    fd = os.open(parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        flags = bytearray(8)
        fcntl.ioctl(fd, 0x80086601, flags)
        wanted = struct.unpack_from('=I', flags)[0] | 0x00800000
        fcntl.ioctl(fd, 0x40086602, struct.pack('=Q', wanted))
        fcntl.ioctl(fd, 0x80086601, flags)
        assert struct.unpack_from('=I', flags)[0] & 0x00800000
    finally:
        os.close(fd)
    result = subprocess.run([CLI, 'create', str(parent / 'volume000.db'), '--module',
        '/cli/dynblk.ko', '--execute'], capture_output=True, text=True)
    assert result.returncode and 'Operation not supported' in result.stderr, result
    assert not list(parent.iterdir()) and not Path('/sys/module/dynblk').exists()
    record('btrfs_nocow_parent_rejected', {'flags': wanted, 'stderr': result.stderr})


def crash_payload(index, version):
    if version == -1:
        return bytes(B)
    seed = hashlib.shake_256(struct.pack('<QQ', index, version))
    if (index // 16) % 2 == 0:
        return seed.digest(64) * (B // 64)
    return seed.digest(B)


def crash_workload(phase):
    volume = ROOT / ('crash-' + phase)
    create(volume, 'lz4' if phase == 'cow' else None, part_limit=2 * MIB)
    blocks = 4096 if phase == 'gc' else 64
    for index in range(0, blocks, 32):
        direct(index * B, b''.join(crash_payload(block, 0) for block in range(index, index + 32)))
    if phase == 'cow':
        assert status()['stored'] < status()['original'] * 3 // 4
        unload()
        mappings = checkpoint(volume)
        for base in (0, 32):
            assert len({mappings[index]['pointer'] for index in range(base, base + 16)}) == 1
            assert all(mappings[index]['decoded_length'] == C and mappings[index]['flags'] == 1
                       for index in range(base, base + 16))
        load(volume)
        record('compressed_shared_blobs_before_cut', {'shared_groups': 2, 'raw_groups': 2})
    state = {'phase': phase, 'versions': [0] * blocks, 'pending': None,
             'capacity': status()['capacity'], 'iterations': 0, 'gc_observed': False,
             'gc_events': 0, 'generation': status()['generation']}
    durable(ROOT / 'crash-state', state)
    announced = False
    # The acknowledged oracle is outside dynblk, fsynced on the independent ext4 lower.
    while True:
        turn = state['iterations']
        if phase == 'checkpoint':
            capacity = state['capacity'] + B
            state['pending'] = {'capacity': capacity}
            durable(ROOT / 'crash-state', state)
            grow(capacity)
            state['capacity'] = capacity
        elif phase == 'gc':
            slots = (blocks - 32) // 32
            index = 32 + (turn % slots) * 32
            version = -1 if (turn // slots) % 2 == 0 else turn + 1
            state['pending'] = {'start': index, 'count': 16, 'version': version}
            durable(ROOT / 'crash-state', state)
            if version == -1:
                discard(index * B, C)
            else:
                direct(index * B, b''.join(crash_payload(block, version) for block in range(index, index + 16)))
            state['versions'][index:index + 16] = [version] * 16
        else:
            index = 32 + ((turn * 16) % (blocks - 32))
            version = state['versions'][index] + 1
            state['pending'] = {'start': index, 'count': 1, 'version': version}
            durable(ROOT / 'crash-state', state)
            direct(index * B, crash_payload(index, version))
            state['versions'][index] = version
        state['pending'] = None
        state['iterations'] += 1
        generation = status()['generation']
        collected = generation > state['generation'] + 1
        state['gc_observed'] |= collected
        state['gc_events'] += int(collected)
        state['generation'] = generation
        durable(ROOT / 'crash-state', state)
        if turn % 64 == 63:
            print('CRASH PROGRESS', phase, state['iterations'], 'gc_events', state['gc_events'], flush=True)
        if turn >= 128 and (phase != 'gc' or collected):
            if not announced:
                kernel_log('dmesg-before-' + phase + '.txt')
                announced = True
            print('DYNBLK ' + phase.upper() + ' CUT READY', flush=True)
        assert turn < 100000, 'Testo did not cut the active workload'


def recover_crash():
    state = json.loads((ROOT / 'crash-state').read_text())
    phase = state['phase']
    assert state['iterations'] >= 129
    assert phase != 'gc' or state['gc_observed'], 'no actual collection observed before cut'
    volume = ROOT / ('crash-' + phase)
    load(volume)
    digest = hashlib.sha256()
    for index, version in enumerate(state['versions']):
        actual = direct(index * B)
        digest.update(actual)
        allowed = [crash_payload(index, version)]
        pending = state['pending']
        if pending and 'start' in pending and pending['start'] <= index < pending['start'] + pending['count']:
            allowed.append(crash_payload(index, pending['version']))
        assert actual in allowed, ('lost acknowledged data or torn COW', phase, index, version)
    allowed_capacity = {state['capacity']}
    if state['pending'] and 'capacity' in state['pending']:
        allowed_capacity.add(state['pending']['capacity'])
    assert status()['capacity'] in allowed_capacity
    unload()
    # A second attach exercises recovered dual-root publication before later reuse.
    load(volume)
    for index in range(32):
        assert direct(index * B) == crash_payload(index, 0)
    unload()
    record('powercut_' + phase, {'acknowledged_iterations': state['iterations'],
                                'pending': state['pending'], 'protected_blocks': 32,
                                'gc_observed': state['gc_observed'],
                                'gc_events': state['gc_events'],
                                'payload_sha256': digest.hexdigest()})
    if phase == 'cow':
        crash_workload('gc')
    elif phase == 'gc':
        crash_workload('checkpoint')
    else:
        kernel_log('dmesg-recovery.txt')
        command('sync')
        partial = any(isinstance(value, dict) and value.get('outcome') in ('UNSUPPORTED', 'FAIL') for value in RESULTS.values())
        print('DYNBLK SELFCONTAINED ' + ('PARTIAL' if partial else 'PASS'), flush=True)


def checkpoint(volume, metadata=None):
    """Read the sole radix layout, independently of the CLI's bounded inspector."""
    assert not Path('/sys/module/dynblk').exists()

    def page(pointer, magic):
        part, offset = pointer >> 32, pointer & 0xFFFFFFFF
        assert offset % B == 0 and offset >= B and part < 64
        with (volume / ('volume%03d.db' % part)).open('rb') as stream:
            stream.seek(offset)
            data = stream.read(B)
        assert len(data) == B and data[:8] == magic
        assert zlib.crc32(data[:4092]) == struct.unpack_from('<I', data, 4092)[0]
        if metadata is not None:
            metadata[pointer] = data
        return data

    roots = [page(B, b'DBROOT01'), page(2 * B, b'DBROOT01')]
    assert roots[0] == roots[1], 'clean detach must leave identical durable roots'
    root = roots[0]
    generation, capacity = struct.unpack_from('<QQ', root, 24)
    tree = struct.unpack_from('<QQII', root, 40)
    transaction = struct.unpack_from('<QQII', root, 64)
    count, pages, height = struct.unpack_from('<QII', root, 88)
    assert height == 4
    if transaction[0]:
        txn = page(transaction[0], b'DBTXN001')
        assert struct.unpack_from('<I', txn, 4092)[0] == transaction[2]
        assert struct.unpack_from('<Q', txn, 24)[0] == transaction[1] == generation
    mappings, seen = {}, set()

    def walk(pointer, level, base, parent_generation):
        location, version, crc, reserved = pointer
        if not location:
            assert pointer == (0, 0, 0, 0)
            return
        assert location not in seen and not reserved and 0 < version <= parent_generation
        seen.add(location)
        data = page(location, b'DBTREE01')
        assert data[8:24] == root[8:24]
        node_generation, node_base, node_level, entries = struct.unpack_from('<QQII', data, 24)
        assert (node_generation, node_base, node_level) == (version, base, level)
        assert struct.unpack_from('<I', data, 4092)[0] == crc
        assert not any(data[48:64])
        found = 0
        if level:
            for index in range(128):
                child = struct.unpack_from('<QQII', data, 64 + index * 24)
                found += bool(child[0])
                walk(child, level - 1, base + (index << (6 + 7 * (level - 1))), version)
            assert not any(data[64 + 128 * 24:4092])
        else:
            for index in range(64):
                value = struct.unpack_from('<QQIIIIII', data, 64 + index * 40)
                payload, published, length, stored_crc, original_crc, flags, decoded_length, decoded_offset = value
                if not payload:
                    assert not any(value)
                    continue
                found += 1
                logical = base + index
                assert logical not in mappings and logical * B < capacity
                assert 0 < published <= version and 1 <= length <= C and flags in (0, 1)
                assert decoded_length in (B, C) and decoded_offset % B == 0
                assert decoded_offset + B <= decoded_length
                assert flags or length == decoded_length
                mappings[logical] = {'pointer': payload, 'length': length, 'flags': flags,
                                     'stored_crc': stored_crc, 'original_crc': original_crc,
                                     'decoded_length': decoded_length, 'decoded_offset': decoded_offset,
                                     'version': published}
            assert not any(data[64 + 64 * 40:4092])
        assert 0 < entries == found

    walk(tree, height - 1, 0, generation)
    assert len(seen) == pages and len(mappings) == count
    assert list(mappings) == sorted(mappings)
    return mappings


@contextlib.contextmanager
def lower_filesystem(kind):
    # Each lower is independent of dynblk and is recreated on its own guest loop.
    image = ROOT / (kind + '-self-lower.img')
    mount = Path('/lower-' + kind)
    mount.mkdir(mode=0o700)
    with image.open('xb') as stream:
        # exFAT derives s_maxbytes from volume geometry; a 2GiB lower rejects
        # the default 4000MiB part cap even though initial storage is only 12KiB.
        stream.truncate((8 if kind in ('vfat', 'exfat') else 2) * 1024 * MIB)
    loop = command('losetup', '--find', '--show', '--sector-size', '512', image).strip()
    assert loop.startswith('/dev/loop') and stat.S_ISBLK(os.stat(loop).st_mode)
    mounted = False
    try:
        assert command('blockdev', '--getss', loop).strip() == '512'
        assert command('blockdev', '--getpbsz', loop).strip() == '512'
        mkfs = {
            'ext2': ('mkfs.ext2', '-F'),
            'ext4': ('mkfs.ext4', '-F', '-E', 'nodiscard,lazy_itable_init=0,lazy_journal_init=0'),
            'btrfs': ('mkfs.btrfs', '-f', '-K'),
            'ntfs3': ('mkfs.ntfs', '-F', '-Q'),
            'vfat': ('mkfs.vfat', '-F', '32', '-S', '512'),
            'exfat': ('mkfs.exfat', '-s', '512'),
        }
        command(*mkfs[kind], loop)
        if kind in ('vfat', 'exfat'):
            # Desktop automounters commonly expose removable media as the
            # logged-in user. DynBlk must not treat synthetic FAT ownership as
            # a kernel security boundary.
            options = 'uid=1000,gid=1000,fmask=0000,dmask=0000'
        elif kind == 'ntfs3':
            options = 'uid=0,gid=0,fmask=0077,dmask=0077'
        else:
            options = 'nodiscard'
        if kind == 'ext2':
            options = 'rw'
        command('mount', '-t', kind, '-o', options, loop, mount)
        mounted = True
        permission_evidence = {}
        if kind == 'ntfs3':
            before = mount.stat()
            command('chmod', '0700', mount)
            after = mount.stat()
            assert after.st_uid == 0 and not after.st_mode & 0o022
            permission_evidence = {'initial_root_mode': oct(stat.S_IMODE(before.st_mode)),
                                   'private_root_mode': oct(stat.S_IMODE(after.st_mode)),
                                   'root_uid': after.st_uid}
        actual = command('findmnt', '-n', '-o', 'FSTYPE,SOURCE,OPTIONS', '--target', mount).strip()
        assert actual.split()[0] == kind and actual.split()[1] == loop, actual
        identity = command('blkid', '-s', 'UUID', '-o', 'value', loop).strip()
        evidence = {'mount': actual, 'kernel': os.uname().release, 'filesystem_uuid': identity}
        evidence.update(permission_evidence)
        if kind == 'ext2':
            config = Path('/kernel.config').read_text().splitlines()
            selected = [line for line in config if line.startswith(('CONFIG_EXT2_FS=', 'CONFIG_EXT4_FS=',
                        'CONFIG_EXT4_USE_FOR_EXT2=')) or line == '# CONFIG_EXT2_FS is not set']
            assert '# CONFIG_EXT2_FS is not set' in selected and 'CONFIG_EXT4_USE_FOR_EXT2=y' in selected
            dependencies = command('modprobe', '--show-depends', 'ext2')
            assert 'ext4.ko' in dependencies and Path('/sys/module/ext4').exists()
            superblock = command('dumpe2fs', '-h', loop)
            features = next(line.split(':', 1)[1].split() for line in superblock.splitlines()
                            if line.startswith('Filesystem features:'))
            assert 'has_journal' not in features and 'extent' not in features
            evidence.update(driver='ext4 module registering ext2 filesystem type', kernel_config=selected,
                            features=features, module_dependencies=dependencies)
        if kind == 'btrfs':
            info = bytearray(1024)
            fd = os.open(mount, os.O_RDONLY | os.O_DIRECTORY)
            try:
                fcntl.ioctl(fd, 0x8400941F, info)
            finally:
                os.close(fd)
            devices = struct.unpack_from('=Q', info, 8)[0]
            assert devices == 1
            topology = command('btrfs', 'filesystem', 'show', '--raw', mount)
            assert 'Total devices 1' in topology and loop in topology
            assert 'nodatacow' not in actual
            evidence.update(devices=devices, topology=topology, cow='normal')
        record('lower_' + kind, evidence)
        yield mount
        assert not Path('/sys/module/dynblk').exists()
        assert command('blkid', '-s', 'UUID', '-o', 'value', loop).strip() == identity
    finally:
        # A failed attached driver must remain intact for diagnostics, not be torn down underneath.
        if not Path('/sys/module/dynblk').exists():
            if mounted:
                command('umount', mount)
            command('losetup', '-d', loop)
            image.unlink()


if __name__ == '__main__':
    safety()
    signal.signal(signal.SIGALRM, io_deadline)
    try:
        if (ROOT / 'crash-state').exists():
            RESULTS = json.loads((ROOT / 'results.json').read_text())
            recover_crash()
        else:
            help_text = cli('--help')
            choices = [line.strip().split()[1] for line in help_text.splitlines()
                       if line.strip().startswith('dynblk ')]
            assert set(choices) == {'create', 'load', 'status', 'grow', 'unload', 'inspect', 'check'}
            assert '--filesystem' not in cli('grow', '--help')
            record('generic_cli', choices)
            control_abi_guards()
            kernel_namespace_guard()
            tree_index_budget_guard()
            mapping_memory_guard()
            functional_matrix()
            record('write_latency_before_cuts', dict(IO_METRICS))
            crash_workload('cow')
    except BaseException:
        traceback.print_exc()
        kernel_log('dmesg-failure.txt')
        command('sync')
        print('DYNBLK FAIL selfcontained', flush=True)
        raise
