"""Small reclaim regressions using the shared production engine and QEMU."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
from .test_engine import ROOT, TOOL

class ReclaimTests(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory(prefix='dynblk-reclaim-test-')
        self.addCleanup(self.work.cleanup)
        self.root = Path(self.work.name)

    def tool(self, *args, no_punch=False):
        env = os.environ.copy()
        if no_punch: env['DYNBLK_TEST_NO_PUNCH'] = '1'
        result = subprocess.run([str(TOOL), *map(str, args)], env=env,
                                text=True, capture_output=True, timeout=40)
        self.assertEqual(result.returncode, 0, (args, result.stdout, result.stderr))
        return result

    def prepare(self, fmt, compressed=False, no_punch=False):
        directory = self.root / (fmt + ('-compressed' if compressed else ''))
        directory.mkdir()
        path = directory / ('disk.vmdk' if fmt == 'vmdk' else 'volume000.db')
        self.tool('create', path, fmt, 64 << 20, 'lz4' if compressed else 'none', 'writeback', no_punch=no_punch)
        self.tool('write', path, 0, 2 << 20, 256, 'writeback', no_punch=no_punch)
        # Sentinel is allocated last, blocking automatic tail truncation.
        self.tool('write', path, 4 << 20, 65536, 83, 'writeback', no_punch=no_punch)
        return path, directory / ('disk-s001.vmdk' if fmt == 'vmdk' else 'volume000.db')

    def verify(self, path, fmt):
        self.tool('read', path, 0, 2 << 20, 0, 'writeback')
        self.tool('read', path, 4 << 20, 65536, 83, 'writeback')
        self.tool('check', path)
        if fmt == 'vmdk' and shutil.which('qemu-io'):
            result = subprocess.run(['qemu-io', '-f', 'vmdk', '-c', 'read -P 83 4194304 65536', str(path)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_automatic_punch_releases_interior_without_relocation(self):
        for fmt in ('dynblk', 'vmdk'):
            with self.subTest(format=fmt):
                path, part = self.prepare(fmt)
                before = part.stat()
                self.tool('discard', path, 0, 2 << 20, 0, 'writeback')
                after = part.stat()
                self.assertEqual(before.st_size, after.st_size)
                self.assertLess(after.st_blocks, before.st_blocks)
                self.verify(path, fmt)

    def test_no_punch_never_falls_back_to_automatic_relocation(self):
        for fmt in ('dynblk', 'vmdk'):
            with self.subTest(format=fmt):
                path, part = self.prepare(fmt, no_punch=True)
                size = part.stat().st_size
                self.tool('discard', path, 0, 2 << 20, 0, 'writeback', no_punch=True)
                result = self.tool('reclaim', path, no_punch=True)
                self.assertIn('moved=0 ', result.stderr)
                self.assertEqual(part.stat().st_size, size)
                result = self.tool('compact', path, no_punch=True)
                self.assertRegex(result.stderr, r'moved=[1-9][0-9]* ')
                self.assertLess(part.stat().st_size, size)
                self.verify(path, fmt)

    def test_native_compressed_compaction_and_vmdk_zero_scan(self):
        path, part = self.prepare('dynblk', compressed=True, no_punch=True)
        self.tool('discard', path, 0, 2 << 20, 0, 'writeback', no_punch=True)
        before = json.loads(self.tool('check', path).stdout)['stored']
        self.tool('compact', path, no_punch=True)
        self.assertEqual(json.loads(self.tool('check', path).stdout)['stored'], before)
        self.verify(path, 'dynblk')
        path, part = self.prepare('vmdk', no_punch=True)
        self.tool('write', path, 0, 2 << 20, 0, 'writeback', no_punch=True)
        result = self.tool('zeroes', path, no_punch=True)
        self.assertIn('unmapped=2097152 ', result.stderr)
        self.tool('compact', path, no_punch=True)
        self.verify(path, 'vmdk')

    def test_cli_requires_explicit_compact_and_execute(self):
        def cli(*args):
            return subprocess.run([str(ROOT / 'dynblk'), *args], capture_output=True, text=True)
        result = cli('reclaim', '/dev/dynblk254', '--json')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), {'dry_run': True, 'compact': False, 'scan_zeroes': False})
        result = cli('reclaim', '/dev/dynblk254', '--compact', '--json')
        self.assertTrue(json.loads(result.stdout)['compact'])
        for args in (('--automatic',), ('--max-steps', '0'), ('--max-steps', '-1')):
            self.assertNotEqual(cli('reclaim', '/dev/dynblk254', *args).returncode, 0)
