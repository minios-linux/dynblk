"""Guest-only utilities for installed-checker and large-volume acceptance."""
import hashlib
import json
import os
from pathlib import Path
import signal
import stat
import struct
import subprocess
import time
import zlib

import selfcontained as suite

B, C, MIB = suite.B, suite.C, suite.MIB
ROOT, DEVICE, CLI = suite.ROOT, suite.DEVICE, suite.CLI


def safety():
    tokens = Path('/proc/cmdline').read_text().split()
    assert 'dynblk_disposable_acceptance=1' in tokens and 'dynblk_selfcontained=1' in tokens
    assert os.geteuid() == 0 and os.uname().release.startswith('6.12.')
    assert Path('/sys/class/block/sda/size').read_text().strip() == Path('/release-sectors').read_text().strip()
    assert not Path('/dev/sdb').exists()
    assert suite.command('blkid', '-s', 'LABEL', '-o', 'value', '/dev/sda').strip() == 'DYNBLK-RELEASE'
    assert suite.command('findmnt', '-n', '-o', 'SOURCE', '--target', ROOT).strip() == '/dev/sda'
    signal.signal(signal.SIGALRM, suite.io_deadline)


def fingerprint(volume):
    assert not Path('/sys/module/dynblk').exists()
    result = {}
    for name in suite.inventory(volume):
        fd = os.open(volume / name, os.O_RDONLY | os.O_NOATIME | os.O_NOFOLLOW)
        try:
            info = os.fstat(fd)
            digest = hashlib.sha256()
            while data := os.read(fd, MIB):
                digest.update(data)
            result[name] = {'size': info.st_size, 'atime_ns': info.st_atime_ns,
                            'mtime_ns': info.st_mtime_ns, 'ctime_ns': info.st_ctime_ns,
                            'sha256': digest.hexdigest()}
        finally:
            os.close(fd)
    return result


def check_preserved(volume, maps=None, stored=None, blobs=None, error=None, timeout=1800):
    assert not Path('/sys/module/dynblk').exists()
    # An old atime makes a missing O_NOATIME observable even with relatime mounts.
    for name in suite.inventory(volume):
        os.utime(volume / name, ns=(1_000_000_000, 2_000_000_000))
    before = fingerprint(volume)
    started = time.monotonic()
    result = subprocess.run([CLI, 'check', str(volume / 'volume000.db'), '--json'],
                            capture_output=True, text=True, timeout=timeout)
    elapsed = time.monotonic() - started
    print('CHECK', volume, result.returncode, 'seconds', round(elapsed, 3), flush=True)
    print(result.stdout, end='', flush=True)
    print(result.stderr, end='', flush=True)
    assert fingerprint(volume) == before, 'check mutated bytes, file sizes or timestamps'
    if error is not None:
        assert result.returncode != 0 and error in result.stderr, result
        assert not result.stdout.strip(), 'failed check emitted success JSON'
        return {'rejected': error, 'seconds': elapsed, 'unchanged': True}
    assert result.returncode == 0, result
    report = json.loads(result.stdout)
    assert report['fully_validated'] is True and report['validation'] == 'passed'
    assert report['validation_scope'] == 'selected-live-state'
    assert report['verified_mapping_count'] == report['mapped_blocks'] == maps
    assert report['verified_live_bytes'] == maps * B
    assert report['verified_tree_pages'] == report['tree_pages']
    if stored is not None:
        assert report['verified_stored_bytes'] == stored
    if blobs is not None:
        assert report['verified_blobs'] == blobs
    return {'report': report, 'seconds': elapsed, 'unchanged': True, 'files': before}


def read_at(volume, location, length):
    fd = os.open(volume / ('volume%03d.db' % (location >> 32)), os.O_RDONLY | os.O_NOATIME)
    try:
        data = os.pread(fd, length, location & 0xffffffff)
        assert len(data) == length
        return data
    finally:
        os.close(fd)


def write_at(volume, location, data):
    assert not Path('/sys/module/dynblk').exists()
    fd = os.open(volume / ('volume%03d.db' % (location >> 32)), os.O_RDWR | os.O_NOATIME)
    try:
        assert os.pwrite(fd, data, location & 0xffffffff) == len(data)
        os.fsync(fd)
    finally:
        os.close(fd)


def alter_leaf(volume, logical, change):
    """Forge a test value while keeping every parent CRC binding valid."""
    root = bytearray(read_at(volume, B, B))

    def rewrite(location, level, base):
        page = bytearray(read_at(volume, location, B))
        assert page[:8] == b'DBTREE01'
        if level == 0:
            change(page, 64 + (logical - base) * 40)
        else:
            span = 1 << (6 + 7 * (level - 1))
            index = (logical - base) // span
            child = struct.unpack_from('<Q', page, 64 + index * 24)[0]
            crc = rewrite(child, level - 1, base + index * span)
            struct.pack_into('<I', page, 64 + index * 24 + 16, crc)
        crc = zlib.crc32(page[:4092])
        struct.pack_into('<I', page, 4092, crc)
        write_at(volume, location, page)
        return crc

    crc = rewrite(struct.unpack_from('<Q', root, 40)[0], 3, 0)
    struct.pack_into('<I', root, 56, crc)
    struct.pack_into('<I', root, 4092, zlib.crc32(root[:4092]))
    write_at(volume, B, root)
    write_at(volume, 2 * B, root)


def finish(marker):
    suite.kernel_log('dmesg-recovery.txt')
    suite.command('sync')
    print(marker, flush=True)
