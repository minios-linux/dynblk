"""Focused detached-copy and mixed compressed/raw crash acceptance."""
import hashlib
import json
import os
import shutil
import signal
import traceback

import selfcontained as suite


def portable_copies():
    source = suite.ROOT / 'portable-source'
    suite.create(source, 'lz4', part_limit=2 * suite.MIB)
    payload = hashlib.shake_256(b'portable-16MiB').digest(16 * suite.MIB)
    for offset in range(0, len(payload), 2 * suite.C):
        suite.direct(offset, payload[offset:offset + 2 * suite.C])
    identity = suite.status()['uuid']
    suite.unload()
    original = suite.hashes(source)
    assert 3 <= len(original) < 64
    hidden = source.with_name('portable-source-hidden')
    for kind in ('ntfs3', 'ext4', 'vfat', 'exfat', 'ext2', 'btrfs'):
        with suite.lower_filesystem(kind) as lower:
            target = lower / 'portable'
            shutil.copytree(source, target, copy_function=shutil.copyfile)
            os.chmod(target, 0o700)
            for path in target.iterdir():
                os.chmod(path, 0o600)
                with path.open('rb') as stream:
                    os.fsync(stream.fileno())
            fd = os.open(target, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
            assert suite.hashes(target) == original
            source.rename(hidden)
            try:
                before = suite.inventory(target)
                suite.load(target)
                assert suite.status()['uuid'] == identity and suite.status()['part_limit'] == 2 * suite.MIB
                assert set(suite.inventory(target)) == set(before), 'attach created unused reserved parts'
                assert suite.direct(0, length=len(payload)) == payload
                replacement = bytes([37]) * suite.C
                suite.direct(0, replacement)
                suite.discard(suite.C, suite.B)
                suite.direct(suite.C + suite.B, bytes(suite.B))
                assert suite.status()['original'] == len(payload) - 2 * suite.B
                expected = replacement + bytes(2 * suite.B) + payload[suite.C + 2 * suite.B:]
                suite.unload()
                mappings = suite.checkpoint(target)
                assert 16 not in mappings and 17 not in mappings
                suite.load(target)
                actual = hashlib.sha256(suite.direct(0, length=len(payload))).hexdigest()
                assert actual == hashlib.sha256(expected).hexdigest()
                suite.unload()
                suite.record('portable_copy_' + kind, {'copied_files': len(original),
                    'copied_hashes': original, 'uuid': identity, 'source_hidden': True,
                    'payload_sha256_after_mutation': actual, 'no_unused_parts_on_attach': True,
                    'zero_write_unmapped': True, 'discard_unmapped': True})
            finally:
                assert not suite.Path('/sys/module/dynblk').exists()
                hidden.rename(source)
    assert suite.hashes(source) == original


suite.safety()
signal.signal(signal.SIGALRM, suite.io_deadline)
try:
    if (suite.ROOT / 'crash-state').exists():
        suite.RESULTS = json.loads((suite.ROOT / 'results.json').read_text())
        suite.recover_crash()
    else:
        suite.record('suite_scope', 'detached-copy and mixed compressed/raw power cuts')
        portable_copies()
        suite.crash_workload('cow')
except BaseException:
    traceback.print_exc()
    suite.kernel_log('dmesg-failure.txt')
    suite.command('sync')
    print('DYNBLK FAIL extra', flush=True)
    raise
