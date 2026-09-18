"""Two real boots: clean package lifecycle, snapshot upgrade and kernel update."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import traceback
import package_lib as p

OLD_KERNEL = '6.12.94+deb13-amd64'
NEW_KERNEL = '6.12.107+deb13-amd64'
STATE = Path('/opt/package-test/upgrade-state.json')


def expected_payload():
    result = bytearray(b''.join(bytes([i + 1]) * 64 + bytes(p.B - 64) for i in range(16)))
    result[5 * p.B:6 * p.B] = b'R' * p.B
    result[2 * p.B:3 * p.B] = bytes(p.B)
    return result


def apt():
    base = Path('/opt/package-test')
    return ['apt-get', '-y', '--no-install-recommends', '-o', 'Dpkg::Use-Pty=0',
            '-o', 'Dir::Etc::sourcelist=' + str(base / 'empty.sources.list'),
            '-o', 'Dir::Etc::sourceparts=' + str(base / 'empty-sources.d'),
            '-o', 'Dir::State::lists=' + str(base / 'empty-apt-lists')]


def guard():
    tokens = Path('/proc/cmdline').read_text().split()
    assert 'dynblk_disposable_acceptance=1' in tokens and 'dynblk_upgrade_test=1' in tokens
    assert os.geteuid() == 0 and os.uname().machine == 'x86_64'
    assert Path('/proc/1/comm').read_text().strip() == 'systemd'
    assert Path('/sys/class/block/sda/size').read_text().strip() == '33554432'
    assert not Path('/dev/sdb').exists()
    assert {v.name for v in Path('/sys/class/net').iterdir()} == {'lo'}
    p.unloaded()
    p.KERNEL = os.uname().release


def first_boot():
    assert p.KERNEL == OLD_KERNEL
    assert not Path('/boot/vmlinuz-' + NEW_KERNEL).exists()
    # The original complete install/remove/purge/hook suite now runs on .94.
    p.main()
    p.collect()
    shutil.copyfile(p.EVIDENCE / 'dmesg.txt', p.EVIDENCE / 'dmesg-old-lifecycle.txt')
    before = p.fingerprints()
    expected = expected_payload()
    old = Path('/opt/package-test/version').read_text().strip()
    new = Path('/opt/package-test/upgrade-version').read_text().strip()
    p.run('09-version-order', 'dpkg', '--compare-versions', new, 'gt', old)
    p.run('09-upgrade', *apt(), 'install', '/opt/upgrade/dynblk.deb', '/opt/upgrade/dynblk-dkms.deb')
    p.installed('09-upgrade', new)
    assert not Path('/usr/src/dynblk-' + old).exists()
    assert not Path('/var/lib/dkms/dynblk/' + old).exists()
    assert p.fingerprints() == before
    p.smoke_existing('09-upgrade-reload', expected)
    p.record('snapshot_upgrade', {'old': old, 'new': new, 'old_registration_removed': True,
                                  'storage_unchanged': True})
    before = p.fingerprints()
    update = sorted(Path('/opt/kernel-update').glob('*.deb'))
    assert update
    p.run('10-kernel-update', *apt(), 'install', *update)
    assert os.uname().release == OLD_KERNEL
    p.installed('10-old-kernel-still-installed', new)
    p.KERNEL = NEW_KERNEL
    p.module_details('10-new-kernel-built-by-hooks', new)
    p.KERNEL = OLD_KERNEL
    assert p.fingerprints() == before
    kernel = Path('/boot/vmlinuz-' + NEW_KERNEL)
    initrd = Path('/boot/initrd.img-' + NEW_KERNEL)
    assert kernel.is_file() and initrd.is_file()
    state = {'old_kernel': OLD_KERNEL, 'new_kernel': NEW_KERNEL, 'version': new,
             'boot_id': Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
             'firmware': 'UEFI' if Path('/sys/firmware/efi').exists() else 'BIOS',
             'files': before, 'payload_sha256': hashlib.sha256(expected).hexdigest(),
             'kernel_sha256': hashlib.sha256(kernel.read_bytes()).hexdigest(),
             'initrd_sha256': hashlib.sha256(initrd.read_bytes()).hexdigest(),
             'mok_sha256': hashlib.sha256(Path('/var/lib/dkms/mok.pub').read_bytes()).hexdigest()}
    with STATE.open('x') as stream:
        json.dump(state, stream, indent=2)
        stream.flush()
        os.fsync(stream.fileno())
    p.record('kernel_update_before_reboot', state)
    p.collect()
    shutil.copyfile(p.EVIDENCE / 'dmesg.txt', p.EVIDENCE / 'dmesg-before-reboot.txt')
    shutil.copyfile(p.EVIDENCE / 'journal.txt', p.EVIDENCE / 'journal-before-reboot.txt')
    subprocess.run(['sync'], check=True)
    return 20


def second_boot():
    state = json.loads(STATE.read_text())
    assert p.KERNEL == NEW_KERNEL
    assert Path('/proc/sys/kernel/random/boot_id').read_text().strip() != state['boot_id']
    assert ('UEFI' if Path('/sys/firmware/efi').exists() else 'BIOS') == state['firmware']
    p.RESULTS.update(json.loads((p.EVIDENCE / 'results.json').read_text()))
    assert p.fingerprints() == state['files']
    assert hashlib.sha256(Path('/var/lib/dkms/mok.pub').read_bytes()).hexdigest() == state['mok_sha256']
    p.installed('11-new-kernel-booted', state['version'])
    expected = expected_payload()
    assert hashlib.sha256(expected).hexdigest() == state['payload_sha256']
    p.smoke_existing('11-new-kernel-reload', expected)
    p.run('12-load', p.CLI, 'load', p.ROOT / 'volume000.db', '--execute')
    # New I/O on the new kernel, not only reading old data or checking vermagic.
    offset = 17 * 1024**3 - p.B
    payload = hashlib.shake_256(b'dynblk-new-kernel-end-probe').digest(p.B)
    p.direct(offset, payload)
    assert p.direct(offset) == payload
    assert p.direct(0, length=len(expected)) == expected
    p.run('12-unload', p.CLI, 'unload', p.DEVICE, '--execute')
    p.run('12-rmmod', 'rmmod', 'dynblk')
    before = p.fingerprints()
    checked = json.loads(p.run('12-full-check', p.CLI, 'check', p.ROOT / 'volume000.db', '--json'))
    assert checked['fully_validated'] is True and checked['verified_mapping_count'] == 16
    assert p.fingerprints() == before
    p.run('12-reload', p.CLI, 'load', p.ROOT / 'volume000.db', '--execute')
    assert p.direct(offset) == payload and p.direct(0, length=len(expected)) == expected
    p.run('12-final-unload', p.CLI, 'unload', p.DEVICE, '--execute')
    p.run('12-final-rmmod', 'rmmod', 'dynblk')
    p.record('new_kernel_write_reload_check', {'offset': offset, 'sha256': hashlib.sha256(payload).hexdigest(),
                                             'check': checked})
    p.run('final-apt-consistency', 'apt-get', 'check')
    p.record('final', {'package_version': state['version'], 'old_kernel': OLD_KERNEL, 'kernel': NEW_KERNEL,
                      'firmware': state['firmware'], 'different_kernel_boot_tested': True,
                      'payload_sha256': state['payload_sha256'], 'installed_not_loaded': True,
                      'mok_preserved': True, 'secure_boot_tested': False})
    p.collect()
    return 0


if __name__ == '__main__':
    result = 1
    try:
        guard()
        result = second_boot() if STATE.exists() else first_boot()
    except BaseException:
        traceback.print_exc()
        p.collect()
    sys.exit(result)
