"""Non-privileged helper argument tests: no devices, mounts or module loading."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / 'mount.dynblk'

class MountHelperTests(unittest.TestCase):
    def helper(self, *args, success=True):
        result = subprocess.run([str(HELPER), *args], text=True, capture_output=True)
        self.assertEqual(result.returncode == 0, success,
                         (args, result.stdout, result.stderr))
        return result

    def test_help(self):
        result = self.helper('--help')
        self.assertIn('mount -t dynblk', result.stdout)
        self.assertIn('last user closes', result.stdout)

    def test_fake_needs_no_source_target_device_or_privileges(self):
        result = self.helper('/absent/volume000.db', '/absent/mount', '-f',
                             '-o', 'ro,inner-fstype=ext4,partition=1')
        self.assertIn('DRY RUN', result.stdout)
        self.assertIn('read-only', result.stdout)
        self.assertIn('selected partition', result.stdout)

    def test_static_binary_supports_the_same_helper_entrypoint(self):
        with tempfile.TemporaryDirectory(prefix="dynblk-static-helper-") as directory:
            helper = Path(directory) / 'mount.dynblk'
            helper.symlink_to(ROOT / 'dynblk-initrd')
            result = subprocess.run([str(helper), '-f', '/absent/volume000.db',
                                     '/absent/mount', '-o', 'ro,inner-fstype=ext4'],
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('read-only', result.stdout)

    def test_fake_honors_last_readwrite_option(self):
        result = self.helper('-f', '/absent/volume000.db', '/absent/mount',
                             '-o', 'ro,rw,inner-fstype=ext4')
        self.assertIn('read-write', result.stdout)

    def test_mutating_and_unsupported_options_are_not_silently_ignored(self):
        for option in ('create', 'size=64MiB', 'loop', 'offset=4096',
                       'sizelimit=1GiB', 'bind', 'rbind', 'remount', 'move',
                       'user', 'users', 'owner', 'group', 'format=qcow2',
                       'cache=invalid', 'inner-fstype=dynblk', 'inner-fstype=noext4',
                       'partition=0', 'partition=-1', 'partition=65536',
                       'partition=1junk', 'module=relative.ko'):
            with self.subTest(option=option):
                self.helper('-f', '/absent/volume000.db', '/absent/mount',
                            '-o', option, success=False)

    def test_each_cache_mode_and_both_formats_are_accepted(self):
        for fmt in ('auto', 'dynblk', 'vmdk'):
            for mode in ('writeback', 'writethrough', 'none', 'directsync', 'unsafe'):
                with self.subTest(format=fmt, cache=mode):
                    result = self.helper('-f', '/absent/disk.vmdk', '/absent/target',
                                         '-o', f'format={fmt},cache={mode},ro')
                    self.assertIn(f'format={fmt} cache={mode}', result.stdout)

    def test_bad_arguments_fail_before_any_attach(self):
        for args in ((), ('relative', '/mnt'), ('/source', 'relative'),
                     ('-f', '/source', '/target', '/extra'),
                     ('-f', '/source', '/target', '-o')):
            with self.subTest(args=args):
                self.helper(*args, success=False)

if __name__ == '__main__':
    unittest.main()
