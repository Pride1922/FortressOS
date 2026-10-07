"""Audit a read-only physical pending capture; Linux repairs only an owned copy."""
import hashlib
import json
import subprocess
import sys
from pathlib import Path
from test_jbd2_replay_host import oracle
from test_nmi_transitions import REPO

def main():
    source=Path(sys.argv[1]).resolve()
    assert source.is_relative_to((REPO/'.codex-remote-attachments/ext4-phase9').resolve())
    assert source.is_file() and source.stat().st_size==33554432
    out=source.parent;digest=hashlib.sha256(source.read_bytes()).hexdigest()
    capture=json.loads((out/'manifest.json').read_text(encoding='utf-8-sig'))
    assert digest==capture['sha256'] and capture['physical_writes'] is False
    assert capture['data_partuuid']=='e68c8e17-8fcd-47fe-b7dd-8f070b5a81d5'
    home=subprocess.run(['debugfs','-R','stat /cut-commit.txt',str(source)],capture_output=True,text=True,check=True)
    (out/'cut-home-stat.log').write_text(home.stdout+home.stderr)
    assert 'File not found' in home.stdout+home.stderr,'unexpected checkpoint/home publication'
    repaired=out/'linux-recovered-copy.ext4'
    assert not repaired.exists(),'refuse to replace previous recovery evidence'
    oracle(source,repaired,out/'cut-linux-recovery.log')
    status=subprocess.run(['debugfs','-R','stat /cut-commit.txt',str(repaired)],capture_output=True,text=True,check=True)
    (out/'cut-linux-stat.log').write_text(status.stdout+status.stderr)
    assert 'Type: regular' in status.stdout and 'Size: 0' in status.stdout
    for name,size,wanted in [('test-1M.bin',1048576,'470952a05336a638e11755d028432cb890c3240d0b33668038a975e7e3b5b4ef'),
                             ('test-16M.bin',16777216,'71b83c780eeb1a2665d5aabf71229490ae069450ef9b88ca91d1229fe17f45c6')]:
        target=out/('audit-'+name)
        subprocess.run(['debugfs','-R',f'dump /{name} {target}',str(repaired)],capture_output=True,check=True)
        data=target.read_bytes();assert len(data)==size and hashlib.sha256(data).hexdigest()==wanted
        target.unlink()
    assert hashlib.sha256(source.read_bytes()).hexdigest()==digest
    (out/'cut-audit.json').write_text(json.dumps({'status':'PASS','source_sha256':digest,'home_file_absent':True,
        'linux_recovered_empty_file':True,'downloads_unchanged':True,'physical_writes':False},indent=2)+'\n')
    print('PASS: pending capture untouched; Linux copy recovers empty file and intact downloads')

if __name__=='__main__':main()
