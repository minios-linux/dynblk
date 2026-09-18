#!/usr/bin/env python3
"""Run only a new disposable VM disk and capture the serial test verdict."""
import argparse
from pathlib import Path
import subprocess
import sys
import time
p = argparse.ArgumentParser()
p.add_argument('artifacts', type=Path)
p.add_argument('--memory', default='512')
a = p.parse_args()
out = a.artifacts.resolve()
iso = out / 'acceptance.iso'
disk = out / 'dual-scratch.qcow2'
log = out / 'dual-serial.log'
if not iso.is_file() or disk.exists() or log.exists():
    raise SystemExit('Require a built ISO and unused scratch/log paths')
subprocess.run(['qemu-img', 'create', '-f', 'qcow2', str(disk), '8G'], check=True)
command = ['qemu-system-x86_64', '-enable-kvm', '-m', a.memory, '-smp', '2',
           '-cdrom', str(iso), '-drive', 'file='+str(disk)+',format=qcow2,if=ide',
           '-boot', 'd', '-display', 'none', '-serial', 'stdio', '-monitor', 'none', '-no-reboot']
result = 1
with log.open('wb') as stream:
    vm = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=stream, stderr=subprocess.STDOUT)
    try:
        until = time.monotonic() + 300
        while time.monotonic() < until and vm.poll() is None:
            time.sleep(.5)
            text = log.read_text(errors='replace')
            if 'DYNBLK DUAL PASS' in text:
                result = 0
                break
            if 'Traceback (most recent call last)' in text or 'DYNBLK FAIL' in text:
                break
    finally:
        if vm.poll() is None:
            vm.terminate()
            try:
                vm.wait(timeout=10)
            except subprocess.TimeoutExpired:
                vm.kill()
                vm.wait()
text = log.read_text(errors='replace')
if any(marker in text for marker in ['BUG:', 'WARNING:', 'Oops:', 'Kernel panic']):
    result = 1
print('\n'.join(line for line in text.splitlines()
                if any(word in line for word in ['PASS', 'FAIL', 'Traceback', 'AssertionError', 'dynblk', 'Error'])))
if result:
    print('\n'.join(text.splitlines()[-40:]))
(out / 'dual-exit-code').write_text(str(result)+'\n')
sys.exit(result)
