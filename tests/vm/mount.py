#!/usr/bin/env python3
"""Disposable-VM-only mount/readonly/autoclear integration acceptance."""
import errno
import fcntl
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import time

assert 'dynblk_disposable_acceptance=1' in Path('/proc/cmdline').read_text()
assert Path('/back').is_mount()
CLI = '/usr/sbin/dynblk'
MOUNT = '/mnt/helper-test'
Path(MOUNT).mkdir(exist_ok=True)
ROOT = Path('/back/mount-cases')
ROOT.mkdir(exist_ok=False)
DEVICE_VOLUMES = {}

def run(*args, ok=True):
    result = subprocess.run(args, text=True, capture_output=True)
    if ok and result.returncode:
        raise AssertionError((args, result.returncode, result.stdout, result.stderr))
    return result

def ctl(*args):
    return run(CLI, *args).stdout.strip()

def gone(device):
    node = Path('/sys/block') / Path(device).name
    for _ in range(100):
        if not node.exists():
            volume = DEVICE_VOLUMES.get(device)
            if volume:
                # Sysfs disappears before final file puts. Before offline
                # corruption/hash tests, wait for actual backing-file release.
                try:
                    with open(volume, 'rb', buffering=0) as backing:
                        fcntl.flock(backing.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    time.sleep(0.05)
                    continue
            return
        time.sleep(0.05)
    raise AssertionError('device did not autoclear: ' + device)

def new_volume(name, partition=False):
    directory = ROOT / name
    directory.mkdir()
    volume = str(directory / 'volume000.db')
    dev = ctl('create', volume, '--size', '64MiB', '--compression', 'lz4',
              '--module', '/cli/dynblk.ko', '--execute')
    target = dev
    if partition:
        table = bytearray(4096)
        struct.pack_into('<B3sB3sII', table, 446, 0, b'\0'*3, 0x83, b'\0'*3, 2048, 129024)
        table[510:512] = b'\x55\xaa'
        with open(dev, 'r+b', buffering=0) as disk:
            disk.write(table)
            os.fsync(disk.fileno())
            fcntl.ioctl(disk.fileno(), 0x125f)
        target += 'p1'
    run('mke2fs', '-q', '-t', 'ext4', '-F', '-E', 'nodiscard', target)
    ctl('unload', dev, '--execute')
    return volume

def mounted(volume, options='inner-fstype=ext4'):
    run('/usr/bin/mount', '-t', 'dynblk', volume, MOUNT, '-o', options)
    dev = run('/usr/bin/findmnt', '-n', '-o', 'SOURCE', '--mountpoint', MOUNT).stdout.strip()
    whole = dev.split('p')[0]
    DEVICE_VOLUMES[whole] = volume
    info = json.loads(ctl('status', whole, '--json'))
    assert info['autoclear'], info
    return whole

def digest(volume):
    return {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
            for p in Path(volume).parent.glob('volume*.db')}

volume = new_volume('plain')
dev = mounted(volume)
Path(MOUNT, 'payload').write_text('mount helper retained data\n')
run('/usr/bin/umount', MOUNT)
gone(dev)
print('PASS mount/umount automatically detaches the owned device', flush=True)
before = digest(volume)
dev = mounted(volume, 'inner-fstype=ext4,ro')
assert Path(MOUNT, 'payload').read_text() == 'mount helper retained data\n'
assert json.loads(ctl('status', dev, '--json'))['read_only']
assert run('/usr/sbin/blockdev', '--getro', dev).stdout.strip() == '1'
assert run(CLI, 'grow', dev, '128MiB', '--execute', ok=False).returncode != 0
run('/usr/bin/umount', MOUNT)
gone(dev)
assert before == digest(volume)
print('PASS read-only mount preserves every backing byte and rejects growth', flush=True)

dev = mounted(volume)
held = os.open(dev, os.O_RDONLY)
run('/usr/bin/umount', MOUNT)
time.sleep(0.1)
assert Path(dev).exists()
os.close(held)
gone(dev)
print('PASS open descriptor delays automatic detach', flush=True)

dev = mounted(volume)
bind = '/mnt/helper-bind'
Path(bind).mkdir()
run('/usr/bin/mount', '--bind', MOUNT, bind)
run('/usr/bin/umount', MOUNT)
assert Path(dev).exists()
assert Path(bind, 'payload').is_file()
run('/usr/bin/umount', bind)
gone(dev)
print('PASS bind mount retains the backing device until its last unmount', flush=True)

dev = mounted(volume)
held = os.open(MOUNT, os.O_RDONLY | os.O_DIRECTORY)
run('/usr/bin/umount', '-l', MOUNT)
assert Path(dev).exists()
os.close(held)
gone(dev)
print('PASS lazy unmount waits for outstanding mount references', flush=True)

assert run('/usr/bin/mount', '-t', 'dynblk', volume, MOUNT,
           '-o', 'inner-fstype=unknownfs', ok=False).returncode
for dev in Path('/sys/block').glob('dynblk*'):
    gone('/dev/' + dev.name)
print('PASS failed mount leaves no attached device', flush=True)

partitioned = new_volume('partitioned', partition=True)
dev = mounted(partitioned, 'inner-fstype=ext4,partition=1')
Path(MOUNT, 'partition-data').write_text('partition mounted\n')
run('/usr/bin/umount', MOUNT)
gone(dev)
print('PASS partition selection and last-close teardown', flush=True)

dev = ctl('load', volume, '--execute')
with open(dev, 'rb', buffering=0) as disk:
    cookie = bytearray(8)
    fcntl.ioctl(disk.fileno(), 0x8008d704, cookie, True)
    value, = struct.unpack('=Q', cookie)
    try:
        fcntl.ioctl(disk.fileno(), 0x4008d705, struct.pack('=Q', value ^ 1))
        raise AssertionError('stale autoclear cookie accepted')
    except OSError as error:
        assert error.errno == errno.ESTALE, error
assert Path(dev).exists()
assert not json.loads(ctl('status', dev, '--json'))['autoclear']
ctl('unload', dev, '--execute')
print('PASS stale autoclear cookie rejected and manual load remains persistent', flush=True)

for _ in range(20):
    dev = mounted(volume)
    run('/usr/bin/umount', MOUNT)
gone(dev)
print('PASS 20 immediate writable mount/unmount cycles', flush=True)

# Read-only attach must not repair even a demonstrably torn second root.
with open(volume, 'r+b', buffering=0) as backing:
    backing.seek(8192)
    old = backing.read(1)
    backing.seek(8192)
    backing.write(bytes([old[0] ^ 1]))
    os.fsync(backing.fileno())
before = digest(volume)
dev = mounted(volume, 'inner-fstype=ext4,ro')
run('/usr/bin/umount', MOUNT)
gone(dev)
assert digest(volume) == before
assert json.loads(ctl('inspect', volume, '--metadata-only', '--json'))['root_repair_needed']
print('PASS read-only attachment leaves a torn root unrepaired', flush=True)

fstab = Path('/etc/fstab')
old_fstab = fstab.read_bytes() if fstab.exists() else None
try:
    fstab.write_text(f'{volume} {MOUNT} dynblk noauto,inner-fstype=ext4,ro 0 0\n')
    run('/usr/bin/mount', MOUNT)
    dev = run('/usr/bin/findmnt', '-n', '-o', 'SOURCE', '--mountpoint', MOUNT).stdout.strip()
    run('/usr/bin/umount', MOUNT)
    gone(dev)
finally:
    if old_fstab is None: fstab.unlink()
    else: fstab.write_bytes(old_fstab)
print('PASS fstab mount/umount', flush=True)

for _ in range(20):
    dev = mounted(volume, 'inner-fstype=ext4,ro')
    run('/usr/bin/umount', MOUNT)
    # Deliberately do not wait: exercise immediate reopen during autoclear.
gone(dev)
print('PASS 20 immediate mount/unmount cycles', flush=True)

for _ in range(100):
    if run('rmmod', 'dynblk', ok=False).returncode == 0:
        break
    time.sleep(0.05)
else:
    raise AssertionError('module remained busy after autoclear')
assert not Path('/sys/module/dynblk').exists()
print('PASS module unload after autoclear drains its worker', flush=True)
print('DYNBLK MOUNT PASS', flush=True)
