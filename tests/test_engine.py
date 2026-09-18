"""Actual shared storage engine, sanitizer binary and independent QEMU oracle."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib
ROOT = Path(__file__).resolve().parents[1]
TOOL = Path(os.environ.get('DYNBLK_ENGINE_TOOL', ROOT / 'tests-storage/.build/engine-tool'))
class EngineTests(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory(prefix='dynblk-engine-')
        self.addCleanup(self.work.cleanup)
        self.root = Path(self.work.name)

    def tool(self, *args, success=True):
        result = subprocess.run([str(TOOL), *map(str, args)], text=True, capture_output=True)
        self.assertEqual(result.returncode == 0, success, (args, result.stdout, result.stderr))
        return json.loads(result.stdout) if success and result.stdout.strip() else result

    def create(self, fmt='dynblk', codec='lz4', mode='writeback', size=64 << 20, name='volume000.db'):
        path = self.root / name
        result = self.tool('create', path, fmt, size, codec, mode)
        return path, result
    def test_native_format_one_and_compression(self):
        path, initial = self.create()
        with path.open('rb') as f:
            header = f.read(4096)
        self.assertEqual(header[:8], b'DBSPRS01')
        self.assertEqual(struct.unpack_from('<I', header, 8)[0], 1)
        self.assertEqual(struct.unpack_from('<I', header, 4092)[0], zlib.crc32(header[:4092]))
        written = self.tool('write', path, 1048576, 1048576, 65, 'writeback')
        self.assertLess(written['stored'], 65536)
        self.assertEqual(written['memory'], initial['memory'])
        self.tool('read', path, 1048576, 1048576, 65, 'writeback')
        self.tool('check', path)

    def test_native_all_cache_modes(self):
        for mode in ('writeback', 'writethrough', 'none', 'directsync', 'unsafe'):
            with self.subTest(cache=mode):
                path, _ = self.create(mode=mode, name=mode + '.dbk')
                result = self.tool('stress', path, 0, 0, 0, mode)
                self.assertLess(result['memory'], 2 * 1048576)
                if mode == 'unsafe': self.assertEqual(result['syncs'], 0)
                self.tool('check', path)

    def test_codecs_and_raw_fallback(self):
        for codec in ('none', 'lz4', 'lz4hc', 'zstd'):
            path, _ = self.create(codec=codec, name=codec + '.dbk')
            self.tool('write', path, 0, 65536, 256, 'writeback')
            self.tool('read', path, 0, 65536, 256, 'writeback')
            self.tool('write', path, 512, 512, 77, 'writeback')
            self.tool('read', path, 512, 512, 77, 'writeback')
            self.tool('check', path)

    def test_discard_reuses_space_and_shrinks_tail(self):
        path, _ = self.create()
        initial = path.stat().st_size
        self.tool('write', path, 0, 1048576, 256, 'writeback')
        expanded = path.stat().st_size
        self.assertGreater(expanded, initial)
        self.tool('discard', path, 0, 1048576, 0, 'writeback')
        self.tool('read', path, 0, 1048576, 0, 'writeback')
        self.assertEqual(path.stat().st_size, initial)
        for _ in range(3):
            self.tool('stress', path)
            self.assertLess(path.stat().st_size, initial + 2 * 1048576)
        self.tool('check', path)

    def test_native_growth_across_new_part(self):
        path, _ = self.create()
        self.tool('grow', path, 1100 * 1048576)
        self.tool('write', path, 1050 * 1048576, 8192, 72, 'none')
        self.tool('read', path, 1050 * 1048576, 8192, 72, 'writeback')
        checked = self.tool('check', path)
        self.assertEqual(checked['capacity'], 1100 * 1048576)
        self.assertEqual(checked['parts'], 2)

    def test_cache_bound_with_many_native_parts(self):
        path, initial = self.create(size=32 << 30)
        for off in (0, (1 << 30) - 4096, 7 << 30, (32 << 30) - 65536):
            self.tool('write', path, off, 8192, 79, 'writeback')
            self.tool('read', path, off, 8192, 79, 'writeback')
        checked = self.tool('check', path)
        self.assertEqual(checked['memory'], initial['memory'])
        self.assertLess(checked['memory'], 2 * 1048576)

    def test_large_create_grow_reload_and_descriptor(self):
        # Sparse payload only: >512-GiB addressing, >999 part names, and a
        # descriptor >64 KiB. No terabyte-scale data-fill benchmark.
        for fmt, codec, name in [('dynblk', 'lz4', 'volume000.db'),
                                  ('vmdk', 'none', 'v' * 105 + '.vmdk')]:
            with self.subTest(format=fmt):
                path, first = self.create(fmt, codec, size=513 << 30, name=name)
                self.tool('write', path, (512 << 30) + 4096, 4096, 81, 'writeback')
                grown = self.tool('grow', path, 1040 << 30)
                self.tool('write', path, (1039 << 30) + 4096, 4096, 82, 'writeback')
                self.tool('read', path, (512 << 30) + 4096, 4096, 81, 'writeback')
                self.tool('read', path, (1039 << 30) + 4096, 4096, 82, 'writeback')
                self.assertEqual(grown['capacity'], 1040 << 30)
                self.assertLess(grown['memory'], 4 << 20)
                if fmt == 'dynblk':
                    self.assertTrue((self.root / 'volume1000.db').exists())
                else:
                    self.assertGreater(path.stat().st_size, 65536)
                    self.qemu('qemu-io', '-f', 'vmdk', '-c',
                              f'read -P 82 {(1039 << 30) + 4096} 4096', path)
                    self.qemu('qemu-io', '-f', 'vmdk', '-c',
                              f'write -P 83 {(1038 << 30) + 4096} 4096', path)
                    self.tool('read', path, (1038 << 30) + 4096, 4096, 83, 'writeback')

    def test_old_experimental_magic_explicitly_rejected(self):
        path = self.root / 'old.db'
        path.write_bytes(b'DBPART01' + bytes(12280))
        result = self.tool('check', path, success=False)
        self.assertIn('-93', result.stderr)

    def test_compressed_corruption_rejected(self):
        path, _ = self.create()
        self.tool('write', path, 0, 65536, 65, 'writeback')
        data = bytearray(path.read_bytes())
        table = struct.unpack_from('<Q', data, 104)[0]
        payload = struct.unpack_from('<Q', data, table)[0]
        self.assertGreater(payload, table)
        data[payload] ^= 1
        path.write_bytes(data)
        self.tool('check', path, success=False)

    def test_header_fallback_readonly_does_not_repair(self):
        path, _ = self.create()
        data = bytearray(path.read_bytes()); data[4096] ^= 1; path.write_bytes(data)
        before = hashlib.sha256(path.read_bytes()).digest()
        self.tool('check', path)
        self.assertEqual(hashlib.sha256(path.read_bytes()).digest(), before)

    def test_overlapping_native_grains_rejected(self):
        path, _ = self.create()
        self.tool('write', path, 0, 131072, 65, 'writeback')
        data = bytearray(path.read_bytes()); table = struct.unpack_from('<Q', data, 104)[0]
        data[table + 16:table + 32] = data[table:table + 16]
        path.write_bytes(data)
        self.tool('check', path, success=False)

    def test_readonly_preserves_atime_and_mtime(self):
        path, _ = self.create()
        self.tool('write', path, 0, 65536, 65, 'writeback')
        os.utime(path, (946684800, 946684800))
        before = path.stat()
        self.tool('check', path)
        after = path.stat()
        self.assertEqual((before.st_atime_ns, before.st_mtime_ns, before.st_size),
                         (after.st_atime_ns, after.st_mtime_ns, after.st_size))

    def qemu(self, *args):
        if not shutil.which(args[0]): self.skipTest('QEMU image tools unavailable')
        result = subprocess.run(list(map(str, args)), text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, (args, result.stdout, result.stderr))
        return result

    def test_vmdk_bidirectional_split_interoperability(self):
        path, _ = self.create('vmdk', 'none', size=5 << 30, name='disk.vmdk')
        self.tool('write', path, (2 << 30) - 4096, 8192, 81, 'writeback')
        self.qemu('qemu-io', '-f', 'vmdk', '-c', f'read -P 81 {(2 << 30) - 4096} 8192', path)
        self.qemu('qemu-io', '-f', 'vmdk', '-c', f'write -P 82 {(4 << 30) - 4096} 8192', path)
        self.tool('read', path, (4 << 30) - 4096, 8192, 82, 'none')
        self.tool('check', path)
        self.qemu('qemu-img', 'check', path)

    def test_vmdk_created_by_qemu(self):
        path = self.root / 'external.vmdk'
        self.qemu('qemu-img', 'create', '-f', 'vmdk', '-o', 'subformat=twoGbMaxExtentSparse', path, '3G')
        self.tool('write', path, 1 << 30, 131072, 88, 'writeback')
        self.qemu('qemu-io', '-f', 'vmdk', '-c', f'read -P 88 {1 << 30} 131072', path)
        self.tool('check', path)

    def test_vmdk_short_cid_preserves_descriptor(self):
        path, _ = self.create('vmdk', 'none', name='short-cid.vmdk')
        original = path.read_text()
        import re
        for cid in ('1', 'abc1234', '01234567'):
            with self.subTest(cid=cid):
                descriptor = re.sub(r'^CID=.*$', 'CID=' + cid, original, flags=re.M)
                path.write_bytes(descriptor.encode() + b'\0\"63\"\n' + bytes(8))
                before = path.read_bytes()
                self.tool('check', path)
                self.assertEqual(path.read_bytes(), before)
                self.tool('write', path, 0, 4096, 65, 'writeback')
                after = path.read_bytes().split(b'\0', 1)[0].decode()
                self.assertEqual(re.sub(r'^CID=.*$', 'CID=ignored', after, flags=re.M),
                                 re.sub(r'^CID=.*$', 'CID=ignored', descriptor, flags=re.M))
                self.tool('check', path)
                self.qemu('qemu-io', '-f', 'vmdk', '-c', 'read -P 65 0 4096', path)

    def test_vmdk_modes_keep_raw_grain_location(self):
        for mode in ('writeback', 'writethrough', 'none', 'directsync', 'unsafe'):
            path, _ = self.create('vmdk', 'none', mode, name=mode + '.vmdk')
            self.tool('write', path, 0, 65536, 65, mode)
            extent = self.root / (mode + '-s001.vmdk')
            before = extent.read_bytes()
            overhead = struct.unpack_from('<Q', before, 64)[0] * 512
            self.tool('write', path, 4096, 4096, 66, mode)
            after = extent.read_bytes()
            self.assertEqual(before[:overhead], after[:overhead])
            self.assertEqual(len(before), len(after))
            self.tool('read', path, 4096, 4096, 66, mode)

    def test_vmdk_growth_remains_readable_by_qemu(self):
        path, _ = self.create('vmdk', 'none', name='grow.vmdk')
        self.tool('grow', path, 3 << 30)
        self.tool('write', path, (3 << 30) - 4096, 4096, 70, 'directsync')
        self.qemu('qemu-io', '-f', 'vmdk', '-c', f'read -P 70 {(3 << 30) - 4096} 4096', path)
        result = json.loads(self.qemu('qemu-img', 'info', '--output=json', path).stdout)
        self.assertEqual(result['virtual-size'], 3 << 30)

    def test_vmdk_rejects_foreign_parent_and_paths(self):
        path, _ = self.create('vmdk', 'none', name='bad.vmdk')
        original = path.read_text()
        for text in (original.replace('parentCID=ffffffff', 'parentCID=12345678'),
                     original.replace('bad-s001.vmdk', '../bad-s001.vmdk'),
                     original.replace('SPARSE', 'FLAT')):
            path.write_text(text)
            self.tool('check', path, success=False)
        path.write_text(original)
        self.tool('check', path)

    def test_vmdk_rejects_payload_reference_into_metadata(self):
        path, _ = self.create('vmdk', 'none', name='metadata.vmdk')
        extent = self.root / 'metadata-s001.vmdk'
        data = bytearray(extent.read_bytes())
        gd = struct.unpack_from('<Q', data, 56)[0] * 512
        gt = struct.unpack_from('<I', data, gd)[0] * 512
        struct.pack_into('<I', data, gt, 8)
        extent.write_bytes(data)
        self.tool('check', path, success=False)

    def test_extent_symlink_and_missing_part_rejected(self):
        path, _ = self.create('vmdk', 'none', size=3 << 30, name='symlink.vmdk')
        part = self.root / 'symlink-s002.vmdk'
        other = self.root / 'renamed.vmdk'
        part.rename(other)
        self.tool('check', path, success=False)
        part.symlink_to(other.name)
        self.tool('check', path, success=False)

if __name__ == '__main__':
    unittest.main()
