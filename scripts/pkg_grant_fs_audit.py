"""Independent Linux debugfs oracle for retained private grant snapshots."""
import datetime
import hashlib
import json
import pathlib
import re
import struct
import subprocess
import sys

ROOT=pathlib.Path(__file__).resolve().parent.parent


def audit(folder):
    folder=folder.resolve();assert folder.is_relative_to((ROOT/'.codex-remote-attachments').resolve())
    out=folder/('oracle-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S'));out.mkdir()
    images=sorted(p for p in folder.glob('*.img') if re.match(r'^\d+-\d+-(charged|refund|recovered|charge-replay|refund-replay)\.img$',p.name))
    assert images
    results=[]
    def h(b):return hashlib.sha256(b).digest()
    for image in images:
        original=hashlib.sha256(image.read_bytes()).hexdigest()
        bs=int(image.name.split('-')[0]);log=[]
        def run(command):
            r=subprocess.run(['debugfs','-R',command,str(image)],text=True,capture_output=True,timeout=30)
            log.append(command+'\n'+r.stdout+r.stderr);assert r.returncode==0
            return r.stdout
        def dump(name):
            dst=out/(image.name+'-'+name+'.dump');run(f'dump /pkg/{name} {dst}');return dst.read_bytes()
        m,l,t,data=(dump(n) for n in ('enrollment.v1','grant.v1','ownership.v1','payload'))
        assert len(m)==512 and len(l)==1024 and len(t)==8192
        assert h(m[:480])==m[480:] and h(l[:992])==l[992:] and h(t[:8160])==t[8160:]
        assert h(t)==l[264:296]
        q=struct.unpack_from('<Q',l,328)[0];seq=struct.unpack_from('<Q',l,24)[0]
        assert q in (0,1) and seq in (1,2,3) and q==(seq==2)
        assert struct.unpack_from('<Q',l,312)[0]+q==8 and data==b'X'*(bs*q)
        if 'charge-replay' in image.name or image.name.endswith('-charged.img'):assert seq==2
        if 'refund-replay' in image.name or image.name.endswith('-refund.img'):assert seq==3
        assert struct.unpack_from('<Q',t,48)[0]==seq
        for name,expected_ino,expected_gen in (
                ('grant.v1',*struct.unpack_from('<II',m,144)),
                ('ownership.v1',*struct.unpack_from('<II',m,152)),
                ('transaction.v1',*struct.unpack_from('<II',m,160)),
                ('payload',*struct.unpack_from('<II',t,260))):
            text=run('stat /pkg/'+name)
            assert int(re.search(r'Inode:\s+(\d+)',text)[1])==expected_ino
            assert int(re.search(r'Generation:\s+(\d+)',text)[1])==expected_gen
            assert int(re.search(r'Mode:\s+([0-7]+)',text)[1],8)==(0o700 if name=='transaction.v1' else 0o600)
            assert re.search(r'User:\s+0\s+Group:\s+0',text)
            if name=='payload':
                assert int(re.search(r'Links:\s+(\d+)',text)[1])==1
                assert int(re.search(r'Size:\s+(\d+)',text)[1])==len(data)
                assert int(re.search(r'Blockcount:\s+(\d+)',text)[1])==q*bs//512
                assert struct.unpack_from('<Q',t,296)[0]==q
        assert hashlib.sha256(image.read_bytes()).hexdigest()==original
        (out/(image.name+'.log')).write_text('\n'.join(log))
        results.append({'image':image.name,'sha256':original,'sequence':seq,'charged_blocks':q,'result':'PASS'})
    (out/'manifest.json').write_text(json.dumps({'result':'PASS','images':results,
        'scope':'read-only Linux debugfs bytes/metadata oracle on retained snapshots; not every callback cut image or boot acceptance'},indent=2)+'\n')
    print('PASS Linux bytes/metadata:',len(results),'snapshots;',out)


if __name__=='__main__':
    for name in sys.argv[1:]:audit(pathlib.Path(name))
