"""Streaming 8GiB raw payload and independent lower-filesystem headroom test."""
import errno
import hashlib
import json
import mmap
import os
from pathlib import Path
import signal
import struct
import subprocess
import time
import traceback
import zlib

import release_common as common
from release_common import B, C, MIB, ROOT, DEVICE, CLI, suite

STEP = 128 * 1024
SIZE = 8 * 1024 * MIB
PART = 4000 * MIB


def payload(index):
    return hashlib.shake_256(b'dynblk-8g-v1' + struct.pack('<Q', index)).digest(STEP)


def attach(volume):
    started = time.monotonic()
    result = subprocess.run([CLI, 'load', str(volume / 'volume000.db'), '--module', '/cli/dynblk.ko', '--execute'],
                            capture_output=True, text=True, timeout=2700)
    print('LARGE ATTACH', time.monotonic() - started, result.stdout, result.stderr, flush=True)
    assert result.returncode == 0
    return suite.status()


def tree_summary(volume):
    assert not Path('/sys/module/dynblk').exists()
    root = common.read_at(volume, B, B)
    assert root == common.read_at(volume, 2 * B, B)
    hashes = {B: hashlib.sha256(root).hexdigest(), 2 * B: hashlib.sha256(root).hexdigest()}
    txn = struct.unpack_from('<Q', root, 64)[0]
    hashes[txn] = hashlib.sha256(common.read_at(volume, txn, B)).hexdigest()
    pending = [(struct.unpack_from('<QQII', root, 40), 3, 0)]
    first, count = {}, 0
    while pending:
        pointer, level, base = pending.pop()
        location, generation, crc, reserved = pointer
        page = common.read_at(volume, location, B)
        assert page[:8] == b'DBTREE01' and not reserved
        assert struct.unpack_from('<Q', page, 24)[0] == generation
        assert zlib.crc32(page[:4092]) == crc == struct.unpack_from('<I', page, 4092)[0]
        hashes[location] = hashlib.sha256(page).hexdigest()
        if level:
            children = []
            for i in range(128):
                child = struct.unpack_from('<QQII', page, 64 + 24 * i)
                if child[0]:
                    children.append((child, level - 1, base + (i << (6 + 7 * (level - 1)))))
            pending.extend(reversed(children))
        else:
            for i in range(64):
                value = struct.unpack_from('<QQIIIIII', page, 64 + 40 * i)
                if not value[0]:
                    continue
                assert value[2] == value[6] == B and value[5] == value[7] == 0
                first.setdefault(value[0] >> 32, (base + i) * B)
                count += 1
    assert len(hashes) - 3 == struct.unpack_from('<I', root, 96)[0]
    assert count == struct.unpack_from('<Q', root, 88)[0]
    return hashes, first


def verify(chunks, edits, label):
    fd = os.open(DEVICE, os.O_RDONLY | os.O_DIRECT)
    buffer = mmap.mmap(-1, STEP)
    actual_hash, expected_hash = hashlib.sha256(), hashlib.sha256()
    started = time.monotonic()
    try:
        for index in range(SIZE // STEP):
            signal.setitimer(signal.ITIMER_REAL, 120)
            assert os.preadv(fd, [buffer], index * STEP) == STEP
            signal.setitimer(signal.ITIMER_REAL, 0)
            actual = buffer[:]
            assert hashlib.sha256(actual).hexdigest() == chunks[index], (label, index)
            expected = bytearray(payload(index))
            for offset, value in edits.items():
                if offset // STEP == index:
                    begin = offset % STEP
                    expected[begin:begin + B] = value
            actual_hash.update(actual)
            expected_hash.update(expected)
            if (index + 1) % 8192 == 0:
                print('LARGE VERIFY', label, (index + 1) * STEP, 'seconds', round(time.monotonic() - started, 3), flush=True)
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        buffer.close()
        os.close(fd)
    assert actual_hash.digest() == expected_hash.digest()
    return {'sha256': actual_hash.hexdigest(), 'seconds': time.monotonic() - started}


def headroom():
    image = ROOT / 'headroom-lower.img'
    with image.open('xb') as stream:
        stream.truncate(256 * MIB)
    loop = suite.command('losetup', '--find', '--show', '--sector-size', '512', image).strip()
    assert loop.startswith('/dev/loop')
    mount = Path('/headroom')
    mount.mkdir(mode=0o700)
    suite.command('mkfs.ext4', '-b', '4096', '-F', '-E', 'nodiscard,lazy_itable_init=0,lazy_journal_init=0', loop)
    suite.command('mount', '-t', 'ext4', loop, mount)
    volume = mount / 'volume'
    suite.create(volume, part_limit=2 * MIB)
    old = hashlib.shake_256(b'headroom-ack').digest(C)
    suite.direct(0, old)
    state = suite.status()
    filler = mount / 'ordinary-filler'
    fd = os.open(filler, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    allocated, failures = 0, []
    try:
        for chunk in (4 * MIB, MIB, B):
            while allocated < 256 * MIB:
                try:
                    os.posix_fallocate(fd, allocated, chunk)
                    allocated += chunk
                except OSError as exc:
                    assert exc.errno == errno.ENOSPC
                    failures.append(exc.errno)
                    break
        os.fsync(fd)
    finally:
        os.close(fd)
    fs = os.statvfs(mount)
    filler_info = filler.stat()
    available = fs.f_bavail * fs.f_frsize
    assert failures and available < 2 * MIB
    try:
        suite.direct(0, b'N' * B)
    except OSError as exc:
        assert exc.errno in (errno.ENOSPC, errno.EIO)
        failure = exc.errno
    else:
        raise AssertionError('headroom mutation unexpectedly succeeded')
    fenced = suite.status(allow_fenced=True)
    assert fenced['flags'] == 1 and fenced['generation'] == state['generation']
    assert fenced['physical'] == state['physical']
    assert suite.direct(0, length=C) == old
    filler.unlink()
    suite.unload()
    suite.load(volume)
    assert suite.direct(0, length=C) == old
    suite.direct(B, b'R' * B)
    expected = old[:B] + b'R' * B + old[2 * B:]
    suite.unload()
    checked = common.check_preserved(volume, 16, C, 16)
    suite.load(volume)
    assert suite.direct(0, length=C) == expected
    suite.unload()
    suite.record('lower_headroom_rejection', {'classification': 'preflight headroom rejection, not observed internal write ENOSPC',
        'ordinary_filler_bytes': filler_info.st_size, 'successful_fallocate_bytes': allocated,
        'ordinary_filler_allocated_bytes': filler_info.st_blocks * 512,
        'ordinary_filler_errno': failures, 'available_bytes': available,
        'dynblk_errno': failure, 'generation_unchanged': True, 'physical_unchanged': True,
        'acknowledged_sha256': hashlib.sha256(old).hexdigest(), 'recovered_check': checked})
    suite.command('umount', mount)
    suite.command('losetup', '-d', loop)
    image.unlink()


def run():
    volume = ROOT / 'large-8g'
    suite.create(volume)
    fd = os.open(DEVICE, os.O_RDWR | os.O_DIRECT)
    buffer = mmap.mmap(-1, STEP)
    chunks, digest = [], hashlib.sha256()
    started, maximum = time.monotonic(), 0.0
    try:
        for index in range(SIZE // STEP):
            data = payload(index)
            assert any(data)
            buffer[:] = data
            before = time.monotonic()
            signal.setitimer(signal.ITIMER_REAL, 120)
            assert os.pwrite(fd, buffer, index * STEP) == STEP
            signal.setitimer(signal.ITIMER_REAL, 0)
            maximum = max(maximum, time.monotonic() - before)
            digest.update(data)
            chunks.append(hashlib.sha256(data).hexdigest())
            if (index + 1) % 256 == 0:
                # The driver commits before each write completes; this is an additional flush.
                os.fsync(fd)
                print('LARGE WRITE', (index + 1) * STEP, 'seconds', round(time.monotonic() - started, 3),
                      'max_write_seconds', round(maximum, 6), flush=True)
            if (index + 1) % 16384 == 0:
                gib = (index + 1) * STEP // (1024 * MIB)
                suite.durable(ROOT / 'large-progress.json', {'acknowledged_bytes': (index + 1) * STEP,
                    'sha256': digest.hexdigest(), 'seconds': time.monotonic() - started})
                print('DYNBLK LARGE ' + str(gib) + 'G WRITTEN', flush=True)
        os.fsync(fd)
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        buffer.close()
        os.close(fd)
    written = suite.status()
    files = suite.inventory(volume)
    assert set(files) == {'volume000.db', 'volume001.db', 'volume002.db'}, files
    assert written['original'] == written['stored'] == SIZE and written['parts'] == 3
    assert written['part_limit'] == PART
    assert all(entry['bytes'] <= PART for entry in files.values())
    assert all(files['volume%03d.db' % i]['bytes'] >= PART - 256 * 1024 for i in (0, 1))
    suite.record('large_write', {'bytes': SIZE, 'sha256': digest.hexdigest(), 'seconds': time.monotonic() - started,
        'max_write_seconds': maximum, 'files': files, 'status': written,
        'durability': 'kernel commits each write before completion; additional fsync every 32MiB'})
    suite.unload()
    old_nodes, first = tree_summary(volume)
    assert set(first) == {0, 1, 2}
    attach(volume)
    initial_read = verify(chunks, {}, 'initial-reload')
    assert initial_read['sha256'] == digest.hexdigest()
    offsets = sorted({first[1] - B, first[1], first[1] + B, first[2] - B, first[2], first[2] + B,
                      PART - B, PART, 2 * PART - B, 2 * PART, SIZE - 2 * B, SIZE - B})
    assert all(0 <= offset < SIZE and offset % B == 0 for offset in offsets)
    edits = {}
    offset = offsets[0]
    replacement = hashlib.shake_256(b'large-update' + struct.pack('<Q', offset)).digest(B)
    physical = suite.file_bytes(volume)
    suite.direct(offset, replacement)
    growth = suite.file_bytes(volume) - physical
    edits[offset] = replacement
    suite.unload()
    new_nodes, _ = tree_summary(volume)
    rewritten = sum(B for key, value in new_nodes.items() if old_nodes.get(key) != value)
    assert rewritten == 7 * B and growth < 128 * 1024, (rewritten, growth)
    suite.durable(ROOT / 'large-metadata.json', {'old_metadata_bytes': len(old_nodes) * B,
        'changed_metadata_bytes': rewritten, 'file_growth': growth, 'first_logical_byte_per_part': first})
    attach(volume)
    for index, offset in enumerate(offsets[1:], 1):
        if index % 2:
            suite.discard(offset, B)
            edits[offset] = bytes(B)
        else:
            replacement = hashlib.shake_256(b'large-update' + struct.pack('<Q', offset)).digest(B)
            suite.direct(offset, replacement)
            edits[offset] = replacement
    for index in {offset // STEP for offset in edits}:
        data = bytearray(payload(index))
        for offset, value in edits.items():
            if offset // STEP == index:
                begin = offset % STEP
                data[begin:begin + B] = value
        chunks[index] = hashlib.sha256(data).hexdigest()
    suite.unload()
    attach(volume)
    final_read = verify(chunks, edits, 'boundary-edits-reload')
    suite.unload()
    unmapped = sum(value == bytes(B) for value in edits.values())
    checked = common.check_preserved(volume, SIZE // B - unmapped, SIZE - unmapped * B, SIZE // B - unmapped, timeout=2700)
    suite.durable(ROOT / 'large-chunks.json', {'generator': 'SHAKE256(dynblk-8g-v1 || uint64-le chunk index)',
        'chunk_bytes': STEP, 'payload_bytes': SIZE, 'initial_sha256': digest.hexdigest(),
        'final_sha256': final_read['sha256'], 'post_edit_chunk_sha256': chunks,
        'edits': {str(offset): {'kind': 'discard' if value == bytes(B) else 'write',
                              'sha256': hashlib.sha256(value).hexdigest()} for offset, value in edits.items()}})
    suite.record('large_verified', {'initial_read': initial_read, 'final_read': final_read,
        'actual_part_transitions': first, 'edit_offsets': offsets, 'changed_metadata_bytes': rewritten,
        'file_growth_after_one_update': growth, 'full_check': checked})
    headroom()
    common.finish('DYNBLK LARGE PASS')


common.safety()
try:
    run()
except BaseException:
    traceback.print_exc()
    suite.kernel_log('dmesg-failure.txt')
    suite.command('sync')
    print('DYNBLK FAIL large', flush=True)
    raise
