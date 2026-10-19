from pathlib import Path
import subprocess,tempfile,re,hashlib,json
from test_ext4_time_host import fixture
from create_ext4_fixtures import ROOT,run
def seal_evidence(evidence):
    assert (evidence/'linux.log').read_text().count('Pass 5:')==8
    text=(evidence/'host.log').read_text();assert 'PASS zero free data blocks' in text and 'short-extra' in text
    paths=['tests/ext4_replace_fault_host.c','tests/ext4_mount_host.c','tests/ext4_time_clock.c','src/fs/ext4_replace.inc','src/fs/ext4.c',
           'src/fs/ext4_permissions.inc','src/fs/ext4_mount_journal.inc','src/fs/ext4_write.inc','src/fs/ext4_transaction.inc']
    (evidence/'manifest.json').write_text(json.dumps({'result':'PASS','source_sha256':{p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in paths},
        'binary_sha256':hashlib.sha256((ROOT/'build/ext4_replace_fault_host').read_bytes()).hexdigest(),
        'scope':'host ASan/UBSan, actual mounted code, four alloc failures, measured read failures, credit exhaustion, races, cache/full disk, credentials, timestamp bounds; eight Linux audits including old/new witnesses'},indent=2)+'\n')
def main():
    evidence=ROOT/'build/ext4-replace-fault';evidence.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='replace-fault-') as tmp:
        folder=Path(tmp);source=fixture(folder,4096,inodes=2048);prefix=folder/'result'
        with (evidence/'host.log').open('w') as log:
            result=subprocess.run([str(ROOT/'build/ext4_replace_fault_host'),str(source),'4096',str(prefix)],stdout=log,stderr=subprocess.STDOUT,timeout=600)
        assert result.returncode==0,(evidence/'host.log').read_text()
        audit=''
        for suffix in ('race','cache','disk-full','credentials','timestamps','short-extra','original-old','activated-new'):
            image=Path(str(prefix)+'-'+suffix+'.img');audit+=run(['e2fsck','-fn',str(image)])
            if suffix=='credentials':
                info=run(['debugfs','-R','stat /target',str(image)]);audit+=info
                assert re.search(r'User:\s+1001\s+Group:\s+1001',info) and re.search(r'Mode:\s+0600',info),info
            if suffix=='timestamps':
                info=run(['debugfs','-R','stat /target',str(image)]);audit+=info
                for field,expected in [('ctime',2147483648),('mtime',1792368000),('crtime',1792368000)]:
                    match=re.search(rf'^\s*{field}:\s+0x([0-9a-f]+):([0-9a-f]+)',info,re.M|re.I);assert match,info
                    low,extra=(int(x,16) for x in match.groups());signed=low if low<2**31 else low-2**32
                    assert signed+(extra&3)*2**32==expected and not extra>>2,info
            if suffix in ('original-old','activated-new'):
                payload=folder/'payload';audit+=run(['debugfs','-R',f'dump /target {payload}',str(image)])
                assert payload.read_bytes()==(b'O' if suffix=='original-old' else b'N')*16;payload.unlink()
                if suffix=='original-old':
                    audit+=run(['debugfs','-R',f'dump /source {payload}',str(image)])
                    assert payload.read_bytes()==b'N'*16;payload.unlink()
                else:
                    info=run(['debugfs','-R','stat /source',str(image)]);assert 'File not found' in info;audit+=info
        (evidence/'linux.log').write_text(audit)
    seal_evidence(evidence)
    print('PASS replacement allocation/read/credit/concurrency/cache/disk-full/credential/timestamp host gates',flush=True)
if __name__=='__main__':main()
