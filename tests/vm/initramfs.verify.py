"""Verify extracted guest evidence after a stopped disposable Testo VM run."""
import json
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

root = Path(sys.argv[1])
firmware = (root / 'firmware').read_text().strip()
boots = []
for phase, count in ((1, 24), (2, 11)):
    data = json.loads((root / 'extracted' / 'evidence' / f'boot-{phase}.json').read_text())
    assert data['phase'] == phase and data['exit_code'] == 0, data
    assert data['checks'] == count and data['firmware'] == firmware, data
    assert re.fullmatch(r'[0-9a-f-]{36}', data['boot_id']), data
    log = (root / 'extracted' / 'evidence' / f'guest-{phase}.log').read_text()
    assert log.count('\nPASS ') == count, (phase, count)
    for codec in ('none', 'lz4', 'lz4hc', 'lzo', 'lzo-rle', 'zstd', 'deflate'):
        suffix = 'busybox-modprobe' if phase == 1 else 'after-cold-boot'
        assert f'PASS codec-{codec}-{suffix}\n' in log
    dmesg = (root / 'extracted' / 'evidence' / f'dmesg-{phase}.log').read_text()
    assert not re.search(r'BUG:|Oops:|Call Trace:|blocked for more than|Kernel panic', dmesg)
    boots.append(data)
assert boots[0]['boot_id'] != boots[1]['boot_id']
junit = ET.parse(root / 'junit.xml').getroot()
assert junit.get('tests') == '1'
for key in ('failures', 'errors', 'skipped'):
    assert junit.get(key) == '0', junit.attrib
assert 'UP-TO-DATE: 0' in (root / 'run.log').read_text()
print(json.dumps({'firmware': firmware, 'boots': boots, 'junit': junit.attrib}, indent=2))
