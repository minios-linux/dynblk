"""Small write-path comparison on the disposable acceptance VM only."""
import json
import mmap
import os
from pathlib import Path
import random
import subprocess
import time

assert 'dynblk_disposable_acceptance=1' in Path('/proc/cmdline').read_text()
assert Path('/back').is_mount()
root = Path('/back/perf-smoke')
root.mkdir(exist_ok=False)
rows = []

def command(*args):
    return subprocess.check_output(list(map(str, args)), text=True).strip()

def measure(directory, label):
    path = directory / 'io.bin'
    size, block = 32 * 1048576, 131072
    memory = mmap.mmap(-1, block)
    memory[:] = os.urandom(block)
    view = memoryview(memory)
    fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR | os.O_DIRECT, 0o600)
    try:
        os.ftruncate(fd, size)
        os.fdatasync(fd)
        rng = random.Random(17)
        random_offsets = [rng.randrange(size // 4096) * 4096 for _ in range(256)]
        for name, bs, offsets in [('sequential', block, range(0, size, block)),
                                  ('random-overwrite', 4096, random_offsets)]:
            begin = time.monotonic()
            with view[:bs] as buf:
                for offset in offsets:
                    assert os.pwritev(fd, [buf], offset) == bs
            os.fdatasync(fd)  # Include final durability, not just cache filling.
            seconds = time.monotonic() - begin
            row = dict(target=label, operation=name, block_bytes=bs,
                       operations=len(offsets), seconds=seconds,
                       MiB_s=bs * len(offsets) / 1048576 / seconds,
                       IOPS=len(offsets) / seconds)
            rows.append(row)
            print('PERF ' + json.dumps(row), flush=True)
    finally:
        os.close(fd)
        view.release()
        memory.close()
        path.unlink()

measure(root, 'direct-ext4')
for fmt, codec in [('dynblk', 'lz4'), ('vmdk', 'none')]:
    directory = root / fmt
    directory.mkdir()
    image = directory / ('disk.vmdk' if fmt == 'vmdk' else 'volume000.db')
    device = command('/usr/sbin/dynblk', 'create', image, '--format', fmt,
                     '--compression', codec, '--cache', 'writeback',
                     '--size', '128MiB', '--execute')
    target = Path('/mnt/perf-' + fmt)
    target.mkdir()
    mounted = False
    try:
        command('mke2fs', '-q', '-t', 'ext4', '-F', '-E',
                'nodiscard,lazy_itable_init=0,lazy_journal_init=0', device)
        command('/usr/bin/mount', '-t', 'ext4', device, target)
        mounted = True
        measure(target, 'ext4-on-' + fmt)
        state = json.loads(command('/usr/sbin/dynblk', 'status', device, '--json'))
        assert not state['fenced'], state
        print('PERF_MEMORY ' + json.dumps(dict(target=fmt,
              engine_memory_bytes=state['engine_memory_bytes'])), flush=True)
    finally:
        if mounted:
            command('/usr/bin/umount', target)
        command('/usr/sbin/dynblk', 'unload', device, '--execute')
(root / 'results.json').write_text(json.dumps(rows, indent=2) + '\n')
