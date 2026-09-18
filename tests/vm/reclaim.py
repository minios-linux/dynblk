#!/usr/bin/env python3
"""Reclaim only newly-created volumes inside the disposable acceptance guest."""
from pathlib import Path
import fcntl
import hashlib
import json
import os
import struct
import subprocess

assert 'dynblk_disposable_acceptance=1' in Path('/proc/cmdline').read_text()
assert Path('/back').is_mount()
ROOT = Path('/back/reclaim-cases')
ROOT.mkdir(exist_ok=False)
CLI = '/usr/sbin/dynblk'

def run(*args):
    result = subprocess.run(list(map(str, args)), capture_output=True, text=True)
    if result.returncode:
        raise AssertionError((args, result.returncode, result.stdout, result.stderr))
    return result.stdout.strip()

def create(base, fmt, name):
    directory = base / name
    directory.mkdir()
    path = directory / ('disk.vmdk' if fmt == 'vmdk' else 'volume000.db')
    dev = run(CLI, 'create', path, '--format', fmt, '--size', '64MiB',
              '--compression', 'lz4' if fmt == 'dynblk' else 'none', '--execute')
    assert dev.startswith('/dev/dynblk')
    return path, dev

def usage(path):
    files = list(path.parent.iterdir())
    return sum(p.stat().st_blocks * 512 for p in files), sum(p.stat().st_size for p in files)

image = ROOT / 'exfat-scratch.img'
with image.open('xb') as f:
    f.truncate(256 << 20)
loop = run('/usr/sbin/losetup', '--find', '--show', image)
run('/usr/sbin/mkfs.exfat', loop)
exfat = ROOT / 'exfat'
exfat.mkdir()
run('/usr/bin/mount', '-t', 'exfat', '-o', 'fmask=0077,dmask=0077', loop, exfat)

for lower, base in [('ext4', ROOT), ('exfat', exfat)]:
    for fmt in ('dynblk', 'vmdk'):
        path, dev = create(base, fmt, lower + '-' + fmt)
        with open(dev, 'r+b', buffering=0) as disk:
            disk.write(os.urandom(2 << 20))
            disk.seek(4 << 20)
            disk.write(b'S' * 65536)
            os.fsync(disk.fileno())
            before = usage(path)
            fcntl.ioctl(disk.fileno(), 0x1277, struct.pack('=QQ', 0, 2 << 20))
            os.fsync(disk.fileno())
            after = usage(path)
            disk.seek(4 << 20)
            assert disk.read(65536) == b'S' * 65536
            assert after[1] == before[1], (lower, fmt, before, after)
            if lower == 'ext4':
                assert after[0] < before[0], (lower, fmt, before, after)
            else:
                assert after[0] == before[0], (lower, fmt, before, after)
            ordinary = json.loads(run(CLI, 'reclaim', dev, '--execute', '--json'))
            assert ordinary['complete'] and ordinary['moved_bytes'] == 0, ordinary
            assert usage(path)[1] == before[1]
            # Compaction is explicitly requested, never a fallback from reclaim.
            compact = json.loads(run(CLI, 'reclaim', dev, '--compact', '--execute', '--json'))
            assert compact['complete'] and compact['moved_bytes'] > 0, compact
            assert usage(path)[1] < before[1], (lower, fmt, compact)
            disk.seek(0)
            assert disk.read(2 << 20) == bytes(2 << 20)
            disk.seek(4 << 20)
            assert disk.read(65536) == b'S' * 65536
        run(CLI, 'unload', dev, '--execute')
        assert json.loads(run(CLI, 'check', path, '--json'))['fully_validated']
        if fmt == 'vmdk':
            run('qemu-io', '-f', 'vmdk', '-c', 'read -P 83 4194304 65536', path)
            run('qemu-io', '-f', 'vmdk', '-c', 'write -P 81 6291456 4096', path)
            dev = run(CLI, 'load', path, '--execute')
            with open(dev, 'rb', buffering=0) as disk:
                disk.seek(6291456)
                assert disk.read(4096) == b'Q' * 4096
            run(CLI, 'unload', dev, '--execute')
        print('PASS reclaim no automatic relocation, explicit compaction, reload', lower, fmt,
              'before=', before, 'after_discard=', after, flush=True)

        # Keep an inner ext4 mounted throughout deletion, trim and reclaim.
        path, dev = create(base, fmt, 'mounted-' + lower + '-' + fmt)
        run('mke2fs', '-q', '-t', 'ext4', '-F', '-E', 'nodiscard', dev)
        target = ROOT / ('inner-' + lower + '-' + fmt)
        target.mkdir()
        run('/usr/bin/mount', '-t', 'ext4', dev, target)
        keep = os.urandom(131072)
        with (target / 'removed').open('wb') as f:
            f.write(os.urandom(2 << 20))
            f.flush(); os.fsync(f.fileno())
        with (target / 'kept').open('wb') as f:
            f.write(keep); f.flush(); os.fsync(f.fileno())
        (target / 'removed').unlink()
        run('fstrim', target)
        ordinary = json.loads(run(CLI, 'reclaim', dev, '--execute', '--json'))
        assert ordinary['moved_bytes'] == 0
        run(CLI, 'reclaim', dev, '--compact', '--execute')
        assert (target / 'kept').read_bytes() == keep
        run('/usr/bin/umount', target)
        run(CLI, 'unload', dev, '--execute')
        run('e2fsck', '-n', run(CLI, 'load', path, '--read-only', '--execute'))
        # Retrieve that single just-opened attachment for cleanup.
        devices = list(Path('/sys/block').glob('dynblk*'))
        assert len(devices) == 1
        run(CLI, 'unload', '/dev/' + devices[0].name, '--execute')
        print('PASS mounted ext4 delete/FITRIM/reclaim', lower, fmt, flush=True)

run('/usr/bin/umount', exfat)
run('/usr/sbin/losetup', '--detach', loop)
print('DYNBLK RECLAIM PASS', flush=True)
