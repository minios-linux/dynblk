"""Non-mutating CLI contracts for the sparse format-1 release."""
import re
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
class NativeTests(unittest.TestCase):
    def cli(self, *args, expected=0):
        r = subprocess.run([str(ROOT / 'dynblk'), *args], text=True, capture_output=True)
        self.assertEqual(r.returncode, expected, (args, r.stdout, r.stderr))
        return r

    def test_help_and_device_only_commands(self):
        self.assertIn('Device management only', self.cli('--help').stdout)
        for cmd in ('mount', 'format', 'resize', 'compact'):
            self.cli(cmd, '/mnt/test', expected=2)

    def test_dryrun_all_cache_modes_and_formats(self):
        for fmt in ('dynblk', 'vmdk'):
            for cache in ('writeback', 'writethrough', 'none', 'directsync', 'unsafe'):
                r = self.cli('create', '/var/lib/dynblk-cli-unused-test.' + fmt,
                             '--format', fmt, '--cache', cache)
                self.assertIn(f'format={fmt} cache={cache}', r.stdout)
                self.assertIn('DRY RUN', r.stdout)

    def test_unsupported_combinations_fail(self):
        for options in (('--format', 'vmdk', '--compression', 'lz4'),
                        ('--format', 'vmdk', '--part-size', '1MiB'),
                        ('--format', 'dynblk', '--subformat', 'twoGbMaxExtentSparse'),
                        ('--format', 'auto'), ('--cache', 'bogus'),
                        ('--format', 'qcow2'), ('--size', '65TiB'),
                        ('--map-memory-mb', '65')):
            self.cli('create', '/var/lib/dynblk-cli-unused-test.dbk', *options, expected=2)

    def test_limits_and_large_capacity_dryrun(self):
        import json
        for fmt, size in [('dynblk', '2TiB'), ('vmdk', '4TiB')]:
            limits = json.loads(self.cli('limits', '--format', fmt, '--json').stdout)
            self.assertEqual(limits['format'], 1)
            self.assertGreater(limits['max_capacity_bytes'], 512 << 30)
            self.cli('create', '/var/lib/dynblk-cli-unused-test.' + fmt,
                     '--format', fmt, '--size', size)
        self.cli('limits', '--format', 'bad', expected=2)
        self.cli('create', '/var/lib/dynblk-cli-unused-test.dbk',
                 '--size', '33GiB', '--part-size', '1MiB', expected=2)

    def test_mapping_cache_is_not_a_fraction_of_ram(self):
        r = self.cli('create', '/var/lib/dynblk-cli-unused-test.dbk', '--map-memory-mb', '1')
        self.assertIn('map_memory_mb=1', r.stdout)
        self.assertNotIn('25%', self.cli('--help').stdout)
        self.cli('create', '/var/lib/dynblk-cli-unused-test.dbk', '--map-memory-mb', '64')

    def test_load_rejects_create_only_options(self):
        for options in (('--size', '16GiB'), ('--compression', 'lz4'), ('--part-size', '1MiB')):
            self.cli('load', '/var/lib/dynblk-cli-unused-test.dbk', *options, expected=2)

    def test_dry_grow_and_unload(self):
        self.assertIn('no filesystem resize', self.cli('grow', '/dev/dynblk17', '32GiB').stdout)
        self.assertIn('no unmount', self.cli('unload', '/dev/dynblk17').stdout)
        self.cli('grow', '/dev/other', '32GiB', expected=2)
        self.cli('grow', '/dev/dynblk256', '32GiB', expected=2)
        self.cli('grow', '/dev/dynblk0p1', '32GiB', expected=2)

    def test_path_safety_does_not_require_root_ownership(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / 'volume000.db'
            created = self.cli('create', str(path))
            self.assertIn('DRY RUN', created.stdout)
            path.write_bytes(b'DBSPRS01' + bytes(8192))
            self.cli('check', str(path), expected=1)
            target = root / 'target'
            target.mkdir()
            link = root / 'link'
            link.symlink_to(target, target_is_directory=True)
            self.cli('create', str(link / 'volume000.db'), expected=1)
        self.cli('create', '/var/lib/../lib/image.dbk', expected=1)
        self.cli('create', '/var/lib/bad name.dbk', expected=1)

    def test_native_dependencies_and_static_binary(self):
        dynamic = subprocess.check_output(['readelf', '-d', str(ROOT / 'dynblk')], text=True)
        self.assertIn('libc.so.6', dynamic)
        for lib in ('liblz4', 'libzstd', 'liblzo', 'libz.so'):
            self.assertNotIn(lib, dynamic)
        program = subprocess.check_output(['readelf', '-l', str(ROOT / 'dynblk-initrd')], text=True)
        self.assertNotIn('Requesting program interpreter', program)
        r = subprocess.run([str(ROOT / 'dynblk-initrd'), '--help'], text=True, capture_output=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('writeback', r.stdout)

    def test_release_mirrors(self):
        version = re.search(r'^dynblk \(([^)]+)\)', (ROOT / 'debian/changelog').read_text()).group(1)
        self.assertIn(f'MODULE_VERSION("{version}")', (ROOT / 'dynblk.c').read_text())
        self.assertIn(f'"Dynblk {version}"', (ROOT / 'debian/dynblk.8').read_text().splitlines()[0])

if __name__ == '__main__': unittest.main()
