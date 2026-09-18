"""Disposable guest acceptance for the shared native/VMDK engines."""
import errno
import fcntl
import hashlib
import json
import mmap
import os
from pathlib import Path
import struct
import subprocess
import time
assert 'dynblk_disposable_acceptance=1' in Path('/proc/cmdline').read_text()
assert Path('/back').is_mount()
ROOT = Path('/back/dual-cases')
ROOT.mkdir(exist_ok=False)
MOUNT = Path('/mnt/dual')
MOUNT.mkdir(exist_ok=True)
CLI = '/usr/sbin/dynblk'
def run(*args, ok=True):
    result = subprocess.run(list(map(str, args)), capture_output=True, text=True)
    if ok and result.returncode:
        raise AssertionError((args, result.returncode, result.stdout, result.stderr))
    return result

def ctl(*args):
    return run(CLI, *args).stdout.strip()

def info(dev):
    return json.loads(ctl('status', dev, '--json'))
def create(name, fmt='dynblk', codec='lz4', cache='writeback', size='64MiB'):
    folder = ROOT / name
    folder.mkdir()
    path = folder / ('disk.vmdk' if fmt == 'vmdk' else 'volume000.db')
    dev = ctl('create', path, '--format', fmt, '--compression', codec, '--cache', cache,
              '--size', size, '--module', '/cli/dynblk.ko', '--execute')
    assert info(dev)['storage_format'] == fmt
    return path, dev

def gone(dev, path):
    for _ in range(150):
        if not Path('/sys/block', Path(dev).name).exists():
            try:
                with path.open('rb', buffering=0) as backing:
                    fcntl.flock(backing.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                return
            except BlockingIOError:
                pass
        time.sleep(.02)
    raise AssertionError('autoclear did not release ' + dev)

def mount_image(path, options):
    run('/usr/bin/mount', '-t', 'dynblk', path, MOUNT, '-o', options)
    dev = run('/usr/bin/findmnt', '-n', '-o', 'SOURCE', '--mountpoint', MOUNT).stdout.strip()
    whole = dev.split('p')[0]
    assert info(whole)['autoclear']
    return whole

def hashes(path):
    return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in path.parent.iterdir() if p.is_file()}

# Check both formats and every cache policy with an actual kernel module.
for fmt, codec in [('dynblk', 'lz4'), ('vmdk', 'none')]:
    for mode in ['writeback', 'writethrough', 'none', 'directsync', 'unsafe']:
        path, dev = create(fmt + '-' + mode, fmt, codec, mode)
        expected = (b'DynBlk dual format verification\n' * 4096)[:65536]
        with open(dev, 'r+b', buffering=0) as disk:
            disk.seek(1048576); disk.write(expected)
            disk.seek(1048576 + 4096); disk.write(b'X' * 4096)
            os.fsync(disk.fileno())
            expected = expected[:4096] + b'X' * 4096 + expected[8192:]
            disk.seek(1048576); assert disk.read(len(expected)) == expected
        current = info(dev)
        assert current['cache'] == mode and not current['fenced'], current
        assert current['engine_memory_bytes'] < 3 * 1048576, current
        ctl('unload', dev, '--execute')
        if fmt == 'vmdk':
            run('qemu-io', '-f', 'vmdk', '-c', 'read -P 88 1052672 4096', path)
            run('qemu-io', '-f', 'vmdk', '-c', 'write -P 89 2097152 4096', path)
        dev = ctl('load', path, '--cache', mode, '--execute')
        with open(dev, 'rb', buffering=0) as disk:
            disk.seek(1048576); assert disk.read(len(expected)) == expected
            if fmt == 'vmdk':
                disk.seek(2097152); assert disk.read(4096) == b'Y' * 4096
        ctl('unload', dev, '--execute')
        checked = json.loads(ctl('check', path, '--json'))
        assert checked['fully_validated'] and checked['format'] == 1
        print('PASS kernel read/write/reload/check', fmt, mode, current['engine_memory_bytes'], flush=True)

# Every currently advertised compressor is exercised through the kernel crypto API.
for codec in ['none', 'lz4hc', 'lzo', 'lzo-rle', 'zstd', 'deflate']:
    path, dev = create('codec-' + codec, codec=codec)
    with open(dev, 'r+b', buffering=0) as disk:
        disk.seek(1048576); disk.write(b'Z' * 131072); os.fsync(disk.fileno())
    ctl('unload', dev, '--execute')
    dev = ctl('load', path, '--execute')
    with open(dev, 'rb', buffering=0) as disk:
        disk.seek(1048576); assert disk.read(131072) == b'Z' * 131072
    ctl('unload', dev, '--execute')
    assert json.loads(ctl('check', path, '--json'))['fully_validated']
    print('PASS kernel compressor', codec, flush=True)

# Whole-disk filesystems, mount helper cache dispatch and read-only preservation.
for fmt, codec in [('dynblk', 'lz4'), ('vmdk', 'none')]:
    path, dev = create('mount-' + fmt, fmt, codec)
    run('mke2fs', '-q', '-t', 'ext4', '-F', '-E', 'nodiscard', dev)
    ctl('unload', dev, '--execute')
    dev = mount_image(path, 'format=' + fmt + ',cache=none,inner-fstype=ext4')
    assert info(dev)['cache'] == 'none'
    (MOUNT / 'payload').write_bytes(b'mounted through ' + fmt.encode())
    run('/usr/bin/umount', MOUNT); gone(dev, path)
    before = hashes(path)
    dev = mount_image(path, 'format=' + fmt + ',cache=writeback,inner-fstype=ext4,ro')
    assert info(dev)['read_only']
    assert (MOUNT / 'payload').read_bytes() == b'mounted through ' + fmt.encode()
    assert run(CLI, 'grow', dev, '96MiB', '--execute', ok=False).returncode
    run('/usr/bin/umount', MOUNT); gone(dev, path)
    assert hashes(path) == before
    print('PASS mount/umount/read-only/cache', fmt, flush=True)

# Real interoperability across the split boundary, in both writer directions.
path, dev = create('split-boundary', 'vmdk', 'none', size='5GiB')
with open(dev, 'r+b', buffering=0) as disk:
    disk.seek((2 << 30) - 4096); disk.write(b'Q' * 8192); os.fsync(disk.fileno())
ctl('unload', dev, '--execute')
run('qemu-io', '-f', 'vmdk', '-c', 'read -P 81 2147479552 8192', path)
run('qemu-io', '-f', 'vmdk', '-c', 'write -P 82 4294963200 8192', path)
dev = ctl('load', path, '--execute')
with open(dev, 'rb', buffering=0) as disk:
    disk.seek((4 << 30) - 4096); assert disk.read(8192) == b'R' * 8192
ctl('unload', dev, '--execute')
assert json.loads(run('qemu-img', 'info', '--output=json', path).stdout)['virtual-size'] == 5 << 30
print('PASS bidirectional split VMDK boundary interoperability', flush=True)

# Sparse large-capacity smoke check: no full-device fill.
for fmt, codec in [('dynblk', 'lz4'), ('vmdk', 'none')]:
    path, dev = create('large-' + fmt, fmt, codec, size='513GiB')
    first = (512 << 30) + 4096
    last = (1039 << 30) + 4096
    with open(dev, 'r+b', buffering=0) as disk:
        disk.seek(first); disk.write(b'A' * 4096); os.fsync(disk.fileno())
    ctl('grow', dev, '1040GiB', '--execute')
    with open(dev, 'r+b', buffering=0) as disk:
        disk.seek(last); disk.write(b'B' * 4096); os.fsync(disk.fileno())
    current = info(dev)
    assert current['capacity_bytes'] == 1040 << 30, current
    assert current['engine_memory_bytes'] < 6 << 20, current
    assert not current['fenced'], current
    ctl('unload', dev, '--execute')
    dev = ctl('load', path, '--read-only', '--execute')
    with open(dev, 'rb', buffering=0) as disk:
        disk.seek(first); assert disk.read(4096) == b'A' * 4096
        disk.seek(last); assert disk.read(4096) == b'B' * 4096
    ctl('unload', dev, '--execute')
    if fmt == 'vmdk':
        run('qemu-io', '-f', 'vmdk', '-c', f'read -P 66 {last} 4096', path)
    else:
        assert (path.parent / 'volume1000.db').is_file()
    print('PASS large create/grow/reload', fmt, current['engine_memory_bytes'], flush=True)

# Grow both formats and persist bytes in the newly-added address range.
for fmt, codec in [('dynblk', 'lz4'), ('vmdk', 'none')]:
    path, dev = create('grow-' + fmt, fmt, codec)
    ctl('grow', dev, '1100MiB', '--execute')
    assert info(dev)['capacity_bytes'] == 1100 * 1048576
    with open(dev, 'r+b', buffering=0) as disk:
        disk.seek(1050 * 1048576); disk.write(b'G' * 4096); os.fsync(disk.fileno())
    ctl('unload', dev, '--execute')
    dev = ctl('load', path, '--execute')
    with open(dev, 'rb', buffering=0) as disk:
        disk.seek(1050 * 1048576); assert disk.read(4096) == b'G' * 4096
    ctl('unload', dev, '--execute')
    if fmt == 'vmdk': run('qemu-io', '-f', 'vmdk', '-c', 'read -P 71 1101004800 4096', path)
    print('PASS grow/reload', fmt, flush=True)

# Logical sector geometry and MBR partition scanning for portable VMDK.
path, dev = create('partition-vmdk', 'vmdk', 'none')
table = bytearray(512)
struct.pack_into('<B3sB3sII', table, 446, 0, b'\0' * 3, 0x83, b'\0' * 3, 2048, 100000)
table[510:512] = b'\x55\xaa'
with open(dev, 'r+b', buffering=0) as disk:
    disk.write(table); os.fsync(disk.fileno()); fcntl.ioctl(disk.fileno(), 0x125f)
run('mke2fs', '-q', '-t', 'ext4', '-F', '-E', 'nodiscard', dev + 'p1')
ctl('unload', dev, '--execute')
dev = mount_image(path, 'format=vmdk,partition=1,inner-fstype=ext4')
(MOUNT / 'partition').write_text('partition is live\n')
run('/usr/bin/umount', MOUNT); gone(dev, path)
print('PASS VMDK 512-byte MBR partition mount', flush=True)

if Path('/reclaim.py').is_file():
    import runpy
    runpy.run_path('/reclaim.py')

# Optional small write-path comparison, with no performance pass/fail threshold.
if Path('/perf.py').is_file():
    import runpy
    runpy.run_path('/perf.py')

assert not list(Path('/sys/block').glob('dynblk*'))
for _ in range(100):
    if run('rmmod', 'dynblk', ok=False).returncode == 0: break
    time.sleep(.05)
else: raise AssertionError('module still busy')
print('DYNBLK DUAL PASS', flush=True)
