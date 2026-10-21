"""Independent read-only capacity potential checks for completed private cases."""
import hashlib
import json
import pathlib
import re
import struct
import subprocess
import sys

ROOT=pathlib.Path(__file__).resolve().parent.parent
PREVIOUS=ROOT/'.codex-remote-attachments/pkg-namespace-kernel-20261021-140820'


def audit(campaign):
    campaign=pathlib.Path(campaign).resolve()
    assert campaign.is_relative_to((ROOT/'.codex-remote-attachments').resolve())
    manifest=json.loads((campaign/'manifest.json').read_text());assert manifest['result']=='PASS'
    results=[]
    for case in manifest['cases']:
        bs,ss,mode=case['block'],case['sector'],case['mode'];label=f'{bs}-{ss}-{mode}'
        source=PREVIOUS/f'{bs}-{ss}-{"shadow" if mode=="recover" else "recover"}.img'
        target=campaign/(label+'.img');original=target.read_bytes();before=hashlib.sha256(original).hexdigest()
        initial_r=tuple(map(int,re.search(r'initial_R=(\d+),(\d+)',(campaign/(label+'.log')).read_text()).groups()))
        source_bytes=source.read_bytes();f0=struct.unpack_from('<II',source_bytes,1036)
        g=tuple(f-r for f,r in zip(f0,initial_r));assert min(g)>=0
        blocks=0;logs=[]
        for i,name in enumerate(('passwd','group','shadow')):
            p=subprocess.run(['debugfs','-R',f'stat /etc/{name}',str(target)],text=True,capture_output=True,check=True)
            logs.append(p.stdout+p.stderr);sectors=int(re.search(r'Blockcount:\s*(\d+)',p.stdout)[1])
            assert sectors%(bs//512)==0;blocks+=sectors//(bs//512)
            assert int(re.search(r'Links:\s*(\d+)',p.stdout)[1])==1
            assert int(re.search(r'Size:\s*(\d+)',p.stdout)[1])==8192
        for path in ('/etc/passwd.publish','/etc/group.publish','/etc/shadow.publish',
                     '/var/lib/fortress/accounts/passwd.new','/var/lib/fortress/accounts/group.new',
                     '/var/lib/fortress/accounts/shadow.new','/var/lib/fortress/accounts/intent.tmp',
                     '/var/lib/fortress/accounts/pending'):
            p=subprocess.run(['debugfs','-R',f'stat {path}',str(target)],text=True,capture_output=True,check=True)
            logs.append(p.stdout+p.stderr);assert 'File not found' in p.stderr
        d=(8192+bs-1)//bs;capacity=9*(d+(1 if d>4 else 0))+2
        final_r=(capacity-blocks,11-3);final_f=struct.unpack_from('<II',original,1036)
        assert tuple(f-r for f,r in zip(final_f,final_r))==g
        assert struct.unpack_from('<I',original,1256)[0]==0 # s_last_orphan
        assert hashlib.sha256(target.read_bytes()).hexdigest()==before
        (campaign/(label+'-capacity-oracle.log')).write_text(''.join(logs))
        results.append(dict(case,initial_R=initial_r,final_R=final_r,unchanged_escrow=g,image_sha256=before))
    (campaign/'capacity-oracle.json').write_text(json.dumps({'result':'PASS','cases':results},indent=2)+'\n')
    print('PASS capacity potential:',len(results),campaign)


if __name__=='__main__':
    for value in sys.argv[1:]:audit(value)
