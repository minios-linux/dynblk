"""Real-kernel codec fixtures and installed full-checker acceptance."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import traceback

import release_common as common
from release_common import B, C, MIB, ROOT, DEVICE, CLI, suite


def run():
    cli_path = Path(CLI)
    info = cli_path.lstat()
    assert stat.S_ISREG(info.st_mode) and info.st_uid == 0 and info.st_nlink == 1
    assert stat.S_IMODE(info.st_mode) & 0o111
    assert not Path('/usr/lib/dynblk').exists()
    suite.record('installed_native_cli', {
        'uid': info.st_uid, 'mode': oct(stat.S_IMODE(info.st_mode)),
        'sha256': hashlib.sha256(cli_path.read_bytes()).hexdigest()})
    fixtures = ROOT / 'fixtures'
    fixtures.mkdir(mode=0o700)
    zeros = b''.join(hashlib.shake_256(bytes([i])).digest(64) + bytes(B - 64) for i in range(16))
    nonzero = b''.join(bytes([i + 1]) * B for i in range(16))
    small_zero, small_nonzero = b'Q' * 64 + bytes(B - 64), b'R' * B
    initial = zeros + nonzero + small_zero + small_nonzero
    for codec in ('none', 'lz4', 'lz4hc', 'lzo', 'lzo-rle', 'zstd', 'deflate'):
        volume = ROOT / ('checker-' + codec)
        suite.create(volume, codec)
        suite.direct(0, zeros)
        suite.direct(C, nonzero)
        suite.direct(2 * C, small_zero)
        suite.direct(2 * C + B, small_nonzero)
        current = suite.status()
        suite.unload()
        mappings = suite.checkpoint(volume)
        assert len(mappings) == 34
        if codec != 'none':
            assert all(value['flags'] == 1 for value in mappings.values())
            assert mappings[0]['decoded_length'] == mappings[16]['decoded_length'] == C
            assert mappings[32]['decoded_length'] == mappings[33]['decoded_length'] == B
        unique = {(v['pointer'], v['version'], v['length']) for v in mappings.values()}
        checked = common.check_preserved(volume, 34, current['stored'], len(unique))
        if codec == 'lzo-rle':
            records = {}
            for label, logical, decoded in (('64k-zero-runs', 0, zeros), ('64k-nonzero', 16, nonzero),
                                            ('4k-zero-runs', 32, small_zero), ('4k-nonzero', 33, small_nonzero)):
                value = mappings[logical]
                encoded = common.read_at(volume, value['pointer'], value['length'])
                assert encoded[:2] == b'\x11\x01' and value['flags'] == 1
                assert 0 < len(encoded) < len(decoded)
                # The native dynblk checker above already decoded this exact
                # kernel-produced blob in-process; retain the wire fixture for
                # forensic/differential evidence without a helper subprocess.
                (fixtures / (label + '.encoded')).write_bytes(encoded)
                (fixtures / (label + '.decoded')).write_bytes(decoded)
                records[label] = {'encoded_length': len(encoded), 'decoded_length': len(decoded),
                    'prefix_hex': encoded[:2].hex(), 'encoded_sha256': hashlib.sha256(encoded).hexdigest(),
                    'decoded_sha256': hashlib.sha256(decoded).hexdigest()}
            shutil.copytree(volume, fixtures / 'lzo-rle-initial')
            (fixtures / 'initial.logical').write_bytes(initial)
            suite.durable(fixtures / 'rle-fixtures.json', records)
            suite.record('actual_kernel_lzo_rle', records)
        suite.load(volume)
        assert suite.direct(0, length=len(initial)) == initial
        replacement = b'W' * B
        suite.direct(5 * B, replacement)
        suite.discard(2 * B, B)
        expected = bytearray(initial)
        expected[5 * B:6 * B] = replacement
        expected[2 * B:3 * B] = bytes(B)
        current = suite.status()
        suite.grow(17 * 1024 * MIB)
        suite.unload()
        mappings = suite.checkpoint(volume)
        unique = {(v['pointer'], v['version'], v['length']) for v in mappings.values()}
        after = common.check_preserved(volume, 33, current['stored'], len(unique))
        suite.load(volume)
        assert suite.status()['capacity'] == 17 * 1024 * MIB
        assert suite.direct(0, length=len(expected)) == expected
        suite.unload()
        if codec == 'lzo-rle':
            shutil.copytree(volume, fixtures / 'lzo-rle-partial')
            (fixtures / 'partial.logical').write_bytes(expected)
            suite.durable(fixtures / 'logical-hashes.json', {
                'initial': hashlib.sha256(initial).hexdigest(),
                'partial': hashlib.sha256(expected).hexdigest(), 'logical_length': len(expected)})
        suite.record('checker_' + codec, {'initial': checked, 'partial': after,
                     'logical_sha256': hashlib.sha256(expected).hexdigest()})

    source = ROOT / 'checker-lzo-rle'
    copied = ROOT / 'checker-copy'
    shutil.copytree(source, copied)
    assert common.fingerprint(source).keys() == common.fingerprint(copied).keys()
    suite.load(copied)
    assert suite.direct(0, length=len(expected)) == expected
    suite.unload()
    suite.record('new_cli_copy', common.check_preserved(copied, 33))

    encoded_bad = ROOT / 'bad-encoded'
    shutil.copytree(source, encoded_bad)
    mapping = suite.checkpoint(encoded_bad)[0]
    raw = bytearray(common.read_at(encoded_bad, mapping['pointer'], mapping['length']))
    raw[-1] ^= 1
    common.write_at(encoded_bad, mapping['pointer'], raw)
    suite.record('reject_encoded', common.check_preserved(encoded_bad, error='stored payload CRC'))

    original_bad = ROOT / 'bad-original-crc'
    shutil.copytree(source, original_bad)
    def change_crc(page, offset):
        value = struct.unpack_from('<I', page, offset + 24)[0]
        struct.pack_into('<I', page, offset + 24, value ^ 1)
    common.alter_leaf(original_bad, 0, change_crc)
    suite.record('reject_original_crc', common.check_preserved(original_bad, error='logical payload CRC'))

    tree_bad = ROOT / 'bad-tree'
    shutil.copytree(source, tree_bad)
    metadata = {}
    suite.checkpoint(tree_bad, metadata)
    leaf = next(location for location, page in metadata.items()
                if page[:8] == b'DBTREE01' and struct.unpack_from('<I', page, 40)[0] == 0)
    page = bytearray(common.read_at(tree_bad, leaf, B))
    page[4000] ^= 1
    common.write_at(tree_bad, leaf, page)
    suite.record('reject_tree', common.check_preserved(tree_bad, error='CRC mismatch'))

    alias = ROOT / 'bad-alias'
    suite.create(alias)
    suite.direct(0, b'A' * B + b'B' * B)
    suite.direct(32 * B, b'Z' * B)
    suite.unload()
    def alias_value(page, offset):
        page[offset:offset + 40] = page[64:104]
    common.alter_leaf(alias, 1, alias_value)
    suite.record('reject_alias', common.check_preserved(alias, error='overlapping distinct payload blobs'))
    suite.command('tar', '-cf', ROOT / 'lzorle-fixtures.tar', '-C', fixtures, '.')
    common.finish('DYNBLK CHECKER PASS')


common.safety()
try:
    run()
except BaseException:
    traceback.print_exc()
    suite.kernel_log('dmesg-failure.txt')
    suite.command('sync')
    print('DYNBLK FAIL checker', flush=True)
    raise
