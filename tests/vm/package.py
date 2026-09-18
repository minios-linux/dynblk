"""Real installed Debian/DKMS lifecycle; runs only on the disposable guest root."""
import fcntl
import hashlib
import json
import mmap
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import time
import traceback

KERNEL = '6.12.107+deb13-amd64'
DEVICE = '/dev/dynblk0'
CLI = '/usr/sbin/dynblk'
ROOT = Path('/srv/private')
EVIDENCE = Path('/evidence')
B = 4096
RESULTS = {}
ENV = dict(os.environ, LC_ALL='C', DEBIAN_FRONTEND='noninteractive')


def run(name, *args, success=True, timeout=900):
    print('RUN', name, *args, flush=True)
    started = time.monotonic()
    result = subprocess.run(list(map(str, args)), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, env=ENV, timeout=timeout)
    (EVIDENCE / (name + '.log')).write_text(result.stdout)
    print(result.stdout, end='', flush=True)
    print('EXIT', name, result.returncode, 'seconds', round(time.monotonic() - started, 3), flush=True)
    assert (result.returncode == 0) == success, (name, result.returncode)
    return result.stdout


def record(name, value=True):
    RESULTS[name] = value
    with (EVIDENCE / 'results.json').open('w') as stream:
        json.dump(RESULTS, stream, indent=2)
        stream.flush()
        os.fsync(stream.fileno())
    print('PASS', name, value, flush=True)


def unloaded():
    assert not Path('/sys/module/dynblk').exists() and not Path(DEVICE).exists(), 'automatic attachment occurred'


def fingerprints():
    unloaded()
    result = {}
    for path in sorted(ROOT.iterdir()):
        info = path.lstat()
        assert stat.S_ISREG(info.st_mode) and info.st_nlink == 1
        fd = os.open(path, os.O_RDONLY | os.O_NOATIME | os.O_NOFOLLOW)
        try:
            digest = hashlib.sha256()
            while data := os.read(fd, 1024 * 1024):
                digest.update(data)
            result[path.name] = {'sha256': digest.hexdigest(), 'size': info.st_size,
                'inode': info.st_ino, 'mode': stat.S_IMODE(info.st_mode),
                'atime_ns': info.st_atime_ns, 'mtime_ns': info.st_mtime_ns, 'ctime_ns': info.st_ctime_ns}
        finally:
            os.close(fd)
    return result


def direct(offset, data=None, length=B):
    fd = os.open(DEVICE, os.O_RDWR | os.O_DIRECT | os.O_DSYNC)
    buffer = mmap.mmap(-1, length if data is None else len(data))
    try:
        if data is None:
            assert os.preadv(fd, [buffer], offset) == length
            return buffer[:]
        buffer[:] = data
        assert os.pwrite(fd, buffer, offset) == len(data)
        os.fsync(fd)
    finally:
        buffer.close()
        os.close(fd)


def module_details(stage, version):
    status_text = run(stage + '-dkms-status', 'dkms', 'status', '-m', 'dynblk', '-v', version)
    assert any(KERNEL in line and ': installed' in line for line in status_text.splitlines())
    filename = run(stage + '-module-path', 'modinfo', '-k', KERNEL, '-F', 'filename', 'dynblk').strip()
    path = Path(filename)
    assert path.is_file() and '/updates/dkms/' in filename and path.name.startswith('dynblk.ko')
    assert run(stage + '-module-name', 'modinfo', '-F', 'name', path).strip() == 'dynblk'
    vermagic = run(stage + '-vermagic', 'modinfo', '-F', 'vermagic', path).strip()
    assert vermagic.split()[0] == KERNEL
    source = Path('/usr/src') / ('dynblk-' + version) / 'dynblk.c'
    expected_source = Path('/opt/package-test/kernel-source.sha256').read_text().strip()
    assert hashlib.sha256(source.read_bytes()).hexdigest() == expected_source
    for log in Path('/var/lib/dkms/dynblk').rglob('make.log'):
        shutil.copyfile(log, EVIDENCE / (stage + '-dkms-' + str(log.relative_to('/var/lib/dkms/dynblk')).replace('/', '_')))
    unloaded()
    record(stage + '_module', {'path': filename, 'name': 'dynblk', 'vermagic': vermagic,
           'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'source_sha256': expected_source})


def installed(stage, version):
    states = run(stage + '-package-state', 'dpkg-query', '-W', '-f=${Package} ${Version} ${db:Status-Status}\n',
                 'dynblk', 'dynblk-dkms')
    assert 'dynblk ' + version + ' installed' in states
    assert 'dynblk-dkms ' + version + ' installed' in states
    assert not run(stage + '-dpkg-verify', 'dpkg', '--verify', 'dynblk', 'dynblk-dkms').strip()
    module_details(stage, version)


def smoke_existing(stage, expected):
    assert run(stage + '-load', CLI, 'load', ROOT / 'volume000.db', '--execute').strip() == DEVICE
    assert direct(0, length=len(expected)) == expected
    current = json.loads(run(stage + '-status', CLI, 'status', DEVICE, '--json'))
    assert current['compression'] == 'lzo-rle' and current['capacity_bytes'] == 17 * 1024**3
    assert current['max_capacity_bytes'] == 512 * 1024**3
    run(stage + '-unload', CLI, 'unload', DEVICE, '--execute')
    run(stage + '-rmmod', 'rmmod', 'dynblk')
    before = fingerprints()
    checked = json.loads(run(stage + '-check', CLI, 'check', ROOT / 'volume000.db', '--json'))
    assert checked['fully_validated'] is True and checked['verified_mapping_count'] == 15
    assert fingerprints() == before
    record(stage + '_data', {'sha256': hashlib.sha256(expected).hexdigest(), 'full_check': checked})


def main():
    tokens = Path('/proc/cmdline').read_text().split()
    assert 'dynblk_disposable_acceptance=1' in tokens and 'dynblk_package_test=1' in tokens
    assert os.geteuid() == 0 and os.uname().release == KERNEL and os.uname().machine == 'x86_64'
    assert Path('/proc/1/comm').read_text().strip() == 'systemd', 'must test a booted root, not chroot'
    assert 'VERSION_ID="13"' in Path('/etc/os-release').read_text()
    assert Path('/sys/class/block/sda/size').read_text().strip() == '33554432'
    assert not Path('/dev/sdb').exists()
    assert {p.name for p in Path('/sys/class/net').iterdir()} == {'lo'}, 'VM must have no network interface'
    record('network_absent', True)
    assert run('root-label', 'blkid', '-s', 'LABEL', '-o', 'value', '/dev/sda').strip() == 'DYNBLK-PKG'
    run('not-chroot', 'systemd-detect-virt', '--chroot', success=False)
    run('dependency-consistency', 'apt-get', 'check')
    run('headers', 'dpkg-query', '-W', 'linux-headers-' + KERNEL, 'linux-headers-amd64')
    run('compiler', 'gcc', '--version')
    run('package-inputs', 'sha256sum', '-c', '/opt/package-test/packages.sha256')
    version = Path('/opt/package-test/version').read_text().strip()
    unloaded()
    assert not Path(CLI).exists() and not Path('/usr/sbin/dkms').exists()
    ROOT.mkdir(mode=0o700, parents=True)
    sentinel = ROOT / 'unrelated.txt'
    sentinel.write_bytes(b'dynblk package lifecycle must preserve this unrelated file\n')
    sentinel_before = hashlib.sha256(sentinel.read_bytes()).hexdigest()
    packages = ['/opt/packages/dynblk.deb', '/opt/packages/dynblk-dkms.deb']
    dependencies = sorted(Path('/opt/dependencies').glob('*.deb'))
    assert dependencies, 'dependency installation must be exercised in the VM'
    # --no-download also suppresses acquisition of local path arguments.
    # An empty source/list set permits local .debs without network fallback.
    apt_lists = Path('/opt/package-test/empty-apt-lists')
    apt_lists.mkdir()
    apt_sources = Path('/opt/package-test/empty.sources.list')
    apt_sources.write_text('')
    apt_parts = Path('/opt/package-test/empty-sources.d')
    apt_parts.mkdir()
    apt = ['apt-get', '-y', '--no-install-recommends',
           '-o', 'Dir::Etc::sourcelist=' + str(apt_sources),
           '-o', 'Dir::Etc::sourceparts=' + str(apt_parts),
           '-o', 'Dir::State::lists=' + str(apt_lists),
           '-o', 'Dpkg::Use-Pty=0', '-o', 'Dpkg::Options::=--debug=2']
    run('01-install', *apt, 'install', *packages, *dependencies)
    installed('01-install', version)
    assert not (ROOT / 'volume000.db').exists()
    assert hashlib.sha256(sentinel.read_bytes()).hexdigest() == sentinel_before
    for name in ('dynblk-dkms.postinst', 'dynblk-dkms.prerm'):
        script = Path('/var/lib/dpkg/info') / name
        assert '# Automatically added by dh_dkms/' in script.read_text()
        shutil.copyfile(script, EVIDENCE / name)
    record('apt_local_install', {'version': version, 'dependency_debs': [p.name for p in dependencies],
           'repository_publication_claim': False, 'no_auto_attach_or_format': True})

    run('02-create', CLI, 'create', ROOT / 'volume000.db', '--compression', 'lzo-rle', '--execute')
    assert sorted(p.name for p in ROOT.iterdir()) == ['unrelated.txt', 'volume000.db']
    assert (ROOT / 'volume000.db').stat().st_size == 12288
    status_json = json.loads(run('02-status', CLI, 'status', DEVICE, '--json'))
    assert status_json['capacity_bytes'] == 16 * 1024**3 and status_json['active_parts'] == 1
    payload = b''.join(bytes([i + 1]) * 64 + bytes(B - 64) for i in range(16))
    direct(0, payload)
    direct(5 * B, b'R' * B)
    fd = os.open(DEVICE, os.O_RDWR)
    try:
        fcntl.ioctl(fd, 0x1277, struct.pack('=QQ', 2 * B, B))
        os.fsync(fd)
    finally:
        os.close(fd)
    expected = bytearray(payload)
    expected[5 * B:6 * B] = b'R' * B
    expected[2 * B:3 * B] = bytes(B)
    assert direct(0, length=len(expected)) == expected
    status_json = json.loads(run('02-compressed-status', CLI, 'status', DEVICE, '--json'))
    assert status_json['stored_bytes'] < status_json['original_bytes']
    run('02-grow', CLI, 'grow', DEVICE, '17GiB', '--execute')
    run('02-unload', CLI, 'unload', DEVICE, '--execute')
    smoke_existing('02-reload', expected)
    before = fingerprints()

    run('03-remove', *apt, 'remove', 'dynblk', 'dynblk-dkms')
    unloaded()
    assert not Path(CLI).exists() and not Path('/usr/src/dynblk-' + version).exists()
    run('03-module-absent', 'modinfo', '-k', KERNEL, 'dynblk', success=False)
    assert fingerprints() == before
    record('remove_preserved_storage', before)

    run('04-reinstall', *apt, 'install', *packages)
    installed('04-reinstall', version)
    assert fingerprints() == before
    smoke_existing('04-reload', expected)
    before = fingerprints()

    run('05-purge', 'dpkg', '--debug=2', '--purge', 'dynblk', 'dynblk-dkms')
    unloaded()
    assert not Path(CLI).exists() and not Path('/usr/src/dynblk-' + version).exists()
    run('05-module-absent', 'modinfo', '-k', KERNEL, 'dynblk', success=False)
    assert fingerprints() == before
    record('purge_preserved_storage', before)

    run('06-reinstall', *apt, 'install', *packages)
    installed('06-reinstall', version)
    assert fingerprints() == before
    smoke_existing('06-reload', expected)
    before = fingerprints()

    # Keep registration while discarding all compiled objects for this kernel.
    # remove would deregister the last kernel and make autoinstall a no-op.
    run('07-unbuild-kernel-artifact', 'dkms', 'unbuild', '-m', 'dynblk', '-v', version, '-k', KERNEL)
    assert Path('/usr/src/dynblk-' + version).is_dir()
    assert Path('/var/lib/dkms/dynblk/' + version + '/source').is_symlink()
    pending = run('07-added-status', 'dkms', 'status', '-m', 'dynblk', '-v', version)
    assert pending.strip() == 'dynblk/' + version + ': added'
    run('07-module-absent', 'modinfo', '-k', KERNEL, 'dynblk', success=False)
    assert fingerprints() == before
    run('08-autoinstall', 'dkms', 'autoinstall', '-k', KERNEL)
    installed('08-autoinstall', version)
    assert fingerprints() == before
    hook = Path('/etc/kernel/header_postinst.d/dkms')
    assert hook.is_file()
    shutil.copyfile(hook, EVIDENCE / 'header-postinst-dkms')
    run('08-unbuild-for-header-hook', 'dkms', 'unbuild', '-m', 'dynblk', '-v', version, '-k', KERNEL)
    run('08-header-module-absent', 'modinfo', '-k', KERNEL, 'dynblk', success=False)
    assert fingerprints() == before
    run('08-header-hook', hook, KERNEL)
    installed('08-header-hook', version)
    unloaded()
    assert fingerprints() == before
    smoke_existing('08-final-reload', expected)
    record('same_kernel_autoinstall', {'kernel': KERNEL, 'different_kernel_boot_tested': False})
    assert hashlib.sha256(sentinel.read_bytes()).hexdigest() == sentinel_before
    run('final-apt-check', 'apt-get', 'check')
    record('final', {'package_version': version, 'kernel': KERNEL, 'payload_sha256': hashlib.sha256(expected).hexdigest(),
                    'unrelated_sha256': sentinel_before, 'installed_not_loaded': True})


def collect():
    for source in ('/var/log/dpkg.log', '/var/log/apt/history.log', '/var/log/apt/term.log'):
        path = Path(source)
        if path.exists():
            shutil.copyfile(path, EVIDENCE / source.strip('/').replace('/', '-'))
    with (EVIDENCE / 'dmesg.txt').open('w') as stream:
        subprocess.run(['dmesg'], stdout=stream)
    with (EVIDENCE / 'journal.txt').open('w') as stream:
        subprocess.run(['journalctl', '-b', '--no-pager'], stdout=stream)
    subprocess.run(['sync'], check=True)


if __name__ == "__main__":
    try:
        main()
        collect()
        print('PACKAGE TEST SCRIPT COMPLETE', flush=True)
    except BaseException:
        traceback.print_exc()
        collect()
        print('PACKAGE TEST SCRIPT ERROR', flush=True)
        raise
