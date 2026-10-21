"""Independent read-only Linux audit of completed lifecycle snapshots."""
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
    campaign=json.loads((folder/'manifest.json').read_text());assert campaign['result']=='PASS'
    out=folder/('oracle-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'));out.mkdir()
    results=[]
    for case in campaign['cases']:
        for suffix in ('refunded','recovered'):
            key=f"{case['block']}-{case['sector']}-{suffix}"
            image=folder/(key+'.img');original=hashlib.sha256(image.read_bytes()).hexdigest();log=[]
            def run(command):
                r=subprocess.run(['debugfs','-R',command,str(image)],text=True,capture_output=True,timeout=30)
                log.append(command+'\n'+r.stdout+r.stderr);assert r.returncode==0
                return r.stdout+r.stderr
            def dump(name):
                dst=out/(key+'-'+name);run(f'dump /pkg/{name} {dst}');return dst.read_bytes()
            l,t=dump('grant.v1'),dump('ownership.v1')
            assert len(l)==1024 and len(t)==8192
            assert hashlib.sha256(l[:992]).digest()==l[992:]
            assert hashlib.sha256(t[:8160]).digest()==t[8160:]
            assert hashlib.sha256(t).digest()==l[264:296]
            assert struct.unpack_from('<QQ',l,312)==(8,1) and struct.unpack_from('<QQ',l,328)==(0,0)
            assert struct.unpack_from('<H',t,258)[0]==3
            assert t[268:272]==bytes(4) and t[288:312]==bytes(24)
            ino,gen=struct.unpack_from('<II',t,260)
            assert f'Inode {ino} is not in use' in run(f'testi <{ino}>')
            text=run(f'stat <{ino}>')
            assert int(re.search(r'Generation:\s+(\d+)',text)[1])==gen
            assert re.search(r'Links:\s+0\s+Blockcount:\s+0',text)
            assert 'File not found' in run('stat /pkg/transaction.v1/unit')
            with image.open('rb') as stream:
                stream.seek(1024+232);assert stream.read(4)==bytes(4)
            assert hashlib.sha256(image.read_bytes()).hexdigest()==original
            (out/(key+'.log')).write_text('\n'.join(log))
            results.append({'image':image.name,'inode':ino,'generation':gen,'sha256':original,'result':'PASS'})
    (out/'manifest.json').write_text(json.dumps({'result':'PASS','images':results,
        'scope':'selected snapshots only: Linux free-inode bit/generation, absent name, empty orphan head, grant hashes/refunds; no boot or every-cut oracle'},indent=2)+'\n')
    print('PASS lifecycle Linux oracle:',len(results),'snapshots;',out)


if __name__=='__main__':
    for name in sys.argv[1:]:audit(pathlib.Path(name))
