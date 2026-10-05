"""Create fragmented fixtures; retain a separate full mounted event inventory."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from create_ext4_fixtures import ROOT
from test_ext4_crash_inventory import command, linux_snapshot


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('fixtures', type=Path)
    args = parser.parse_args()
    source = args.fixtures.resolve()
    evidence = Path(os.environ.get('FORTRESS_EXT4_CRASH_EVIDENCE', ROOT/'.codex-remote-attachments/ext4-phase9')).resolve()
    assert evidence.is_relative_to((ROOT/'.codex-remote-attachments').resolve())
    assert source.is_relative_to((ROOT/'.codex-remote-attachments').resolve())
    assert (source/'manifest.json').is_file()
    out = Path(tempfile.mkdtemp(prefix='fragment-', dir=evidence))
    records = []
    for bs in (1024, 2048, 4096):
        for placement in ('normal', 'wrap'):
            initial = source/f'{bs}-{placement}.img'
            image = out/initial.name
            argv = [str(evidence/'bin/ext4_crash_fragment_host'), str(initial), str(image)]
            log = out/f'{bs}-{placement}.log'
            command(argv, log)
            linux_snapshot(image, bs, 'sync', log)
            text = command(['debugfs', '-R', 'stat /target.bin', str(image)], log)
            # Three one-block extents in the inode root. Growth into the freed
            # interleaving holes must exceed its four-entry capacity.
            entries = re.findall(r'\((\d+)\):\d+', text)
            assert entries == ['0', '1', '2'], text
            records.append({'block': bs, 'placement': placement, 'argv': argv,
                            'source_sha256': hashlib.sha256(initial.read_bytes()).hexdigest(),
                            'sha256': hashlib.sha256(image.read_bytes()).hexdigest()})
    (out/'manifest.json').write_text(json.dumps({'cases': records, 'errors': []}, indent=2)+'\n')
    print(f'Fragmented fixtures PASS 6/6: {out}', flush=True)
    subprocess.run(['python3', str(ROOT/'scripts/test_ext4_crash_inventory.py'), str(out)], check=True)


if __name__ == '__main__': main()
