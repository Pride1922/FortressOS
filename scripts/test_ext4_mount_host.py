"""Phase 8.5 actual mounted VFS, disposable regular-file journal fixtures."""
import concurrent.futures,hashlib,json,struct,tempfile,subprocess,sys,os,re
from pathlib import Path
from create_ext4_fixtures import ROOT,run
from test_jbd2_replay_host import FEATURES,blocks,be,put,seal,sbseal,oracle,crc
EVIDENCE=Path(os.environ.get('FORTRESS_EXT4_EVIDENCE',ROOT/'.codex-remote-attachments/ext4-phase8-5'))

def audit_bytes(image,suffix,bs):
    linux=Path(str(image)+'.linux.img');log=Path(str(image)+'.linux.log')
    if suffix in ('lifecycle-clean','activation-clean','recovery-clean','freeze-recovered','directory-cache-clean'):
        data=image.read_bytes()
        assert struct.unpack_from('<H',data,1082)[0]==1
        assert struct.unpack_from('<I',data,1120)[0]==0x42
        assert struct.unpack_from('<I',data,1256)[0]==0
        assert struct.unpack_from('<I',data,2044)[0]==crc(0xffffffff,data[1024:2044])
        journal=blocks(image,f'<{struct.unpack_from("<I",data,1248)[0]}>')
        assert not be(data,journal[0]*bs+28),'clean image left a nonempty journal'
        js=bytearray(data[journal[0]*bs:journal[0]*bs+1024]);stored=be(js,252);put(js,252,0)
        assert stored==crc(0xffffffff,js),'journal superblock checksum'
    expected={'lifecycle-clean':[('/sub/persist.bin',b'P'*(bs+17))],
              'activation-clean':[('/target.bin',b'O'*(3*bs))],
              'freeze-pending':[('/target.bin',b'O'*(3*bs)+b'F'*bs)],
              'freeze-recovered':[('/target.bin',b'O'*(3*bs)+b'F'*bs)],
              'directory-cache-clean':[('/growth/r'+f'{(bs-36)//68-1:03d}'+'x'*56,b'')]}
    text=log.read_text()
    for index,(name,payload) in enumerate(expected.get(suffix,[])):
        target=Path(str(image)+f'.bytes-{index}')
        target.unlink(missing_ok=True)
        text+=run(['debugfs','-R',f'dump {name} {target}',str(linux)])
        assert target.read_bytes()==payload,f'Linux byte mismatch: {image} {name}'
    if suffix in ('lifecycle-clean','pending-seed','recovery-clean'):
        result=run(['debugfs','-R','stat /target.bin',str(linux)])
        assert 'File not found' in result,f'Linux namespace mismatch: {image}'
        text+=result
    if suffix=='directory-cache-clean':
        result=run(['debugfs','-R','stat /growth',str(linux)])
        assert int(re.search(r'Size:\s+(\d+)',result)[1])==2*bs
        text+=result
        for name in ('move.bin','empty'):
            result=run(['debugfs','-R',f'stat /{name}',str(linux)])
            assert 'File not found' in result;text+=result
    log.write_text(text)

def audit_completed(root):
    records=json.loads((root/'manifest.json').read_text());assert len(records)==12
    checked=[]
    for record in records:
        bs=record['block'];ss=record['sector'];placement='wrap' if record['wrap'] else 'normal'
        for suffix in ('lifecycle-clean','pending-seed','activation-clean','recovery-clean','freeze-pending','freeze-recovered'):
            image=root/f'{bs}-{placement}-{ss}-{suffix}.img';audit_bytes(image,suffix,bs)
            checked.append({'image':str(image),'sha256':hashlib.sha256(image.read_bytes()).hexdigest(),
                'linux_sha256':hashlib.sha256(Path(str(image)+'.linux.img').read_bytes()).hexdigest()})
    (root/'byte-audit-manifest.json').write_text(json.dumps({'argv':sys.argv,'images':checked},indent=2)+'\n')
    print(f'EXT4 mount independent clean/JBD2-empty/Linux namespace and byte audits PASS 72/72: {root}')

def smoke(root):
    assert root.resolve().is_relative_to(EVIDENCE.resolve())
    records=[{'block':bs,'sector':ss,'wrap':wrap} for bs in (1024,2048,4096)
             for wrap in (False,True) for ss in (512,4096)]
    out=Path(tempfile.mkdtemp(prefix='latest-vfs-',dir=root.parent));checked=[]
    for record in records:
        bs=record['block'];ss=record['sector'];placement='wrap' if record['wrap'] else 'normal'
        source=root/f'{bs}-{placement}.img';prefix=out/f'{bs}-{placement}-{ss}'
        cmd=[str(EVIDENCE/'bin/ext4_mount_host_smoke'),str(source),str(ss),str(prefix),'--smoke']
        with Path(str(prefix)+'.log').open('w') as stream:
            result=subprocess.run(cmd,stdout=stream,stderr=subprocess.STDOUT,timeout=180)
        assert result.returncode==0,f'retained {prefix}.log'
        image=Path(str(prefix)+'-directory-cache-clean.img');linux=Path(str(image)+'.linux.img')
        oracle(image,linux,Path(str(image)+'.linux.log'));audit_bytes(image,'directory-cache-clean',bs)
        checked.append({'argv':cmd,'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest()})
    (out/'manifest.json').write_text(json.dumps({'cases':checked,
        'vfs_sha256':hashlib.sha256((ROOT/'src/fs/vfs.c').read_bytes()).hexdigest()},indent=2)+'\n')
    print(f'EXT4 latest VFS mount/lifetime/recovery/admission/freeze/concurrency smoke PASS 12/12: {out}')

def profile(out,source,bs,ss,wrap):
    prefix=out/f'{source.stem}-{ss}';cmd=[str(EVIDENCE/'bin/ext4_mount_host'),str(source),str(ss),str(prefix)]
    logfile=out/f'{prefix.name}.log'
    with logfile.open('w') as stream:result=subprocess.run(cmd,stdout=stream,stderr=subprocess.STDOUT,timeout=900)
    text=logfile.read_text();print(text,end='',flush=True);assert result.returncode==0,f'{cmd}: retained {logfile}'
    for suffix in ('lifecycle-clean','pending-seed','activation-clean','recovery-clean','freeze-pending','freeze-recovered'):
        image=Path(str(prefix)+f'-{suffix}.img')
        oracle(image,Path(str(image)+'.linux.img'),Path(str(image)+'.linux.log'))
        audit_bytes(image,suffix,bs)
    return {'block':bs,'sector':ss,'wrap':wrap,'argv':cmd,'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest()}

def main():
    parent=EVIDENCE;parent.mkdir(parents=True,exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='host-',dir=parent));records=[]
    jobs=[]
    for bs in (1024,2048,4096):
        base=out/f'base-{bs}.img'
        with base.open('xb') as f:f.truncate(32*1024*1024)
        log=run(['mke2fs','-q','-t','ext4','-b',str(bs),'-I','256','-O',FEATURES,
            '-E','lazy_itable_init=0','-m','0','-g','4096','-N','512','-J','size=4',str(base)])
        seed=out/f'seed-{bs}.bin';seed.write_bytes(b'O'*(3*bs))
        commands=out/f'initialize-{bs}.commands';commands.write_text(f'write {seed} /target.bin\njournal_open -c -v 3\njournal_close\n')
        log+=run(['debugfs','-w','-f',str(commands),str(base)])
        data=bytearray(base.read_bytes());ino=struct.unpack_from('<I',data,1248)[0];jmap=blocks(base,f'<{ino}>')
        js=bytearray(data[jmap[0]*bs:(jmap[0]+1)*bs]);assert be(js,40) in (0x10,0x11)
        put(js,28,0);put(js,88,1);put(js,40,0x11)
        struct.pack_into('<I',data,1120,0x42);struct.pack_into('<H',data,1082,1);sbseal(data)
        for wrap in (False,True):
            initial=bytearray(data);put(js,24,0xffffffff if wrap else 1);put(js,88,len(jmap)-2 if wrap else 1)
            head=bytearray(js[:1024]);seal(head,252,0xffffffff);js[:1024]=head;initial[jmap[0]*bs:(jmap[0]+1)*bs]=js
            source=out/f'{bs}-{"wrap" if wrap else "normal"}.img';source.write_bytes(initial)
            for ss in (512,4096):
                jobs.append((out,source,bs,ss,wrap))
        (out/f'generation-{bs}.log').write_text(log)
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        records=list(pool.map(lambda args:profile(*args),jobs))
    (out/'manifest.json').write_text(json.dumps(records,indent=2)+'\n')
    (out/'source-context.json').write_text(json.dumps({'argv':sys.argv,
        'binary_sha256':hashlib.sha256((EVIDENCE/'bin/ext4_mount_host').read_bytes()).hexdigest(),
        'vfs_sha256':hashlib.sha256((ROOT/'src/fs/vfs.c').read_bytes()).hexdigest()},indent=2)+'\n')
    print(f'EXT4 mount host PASS 12/12; evidence: {out}')
if __name__=='__main__':
    if len(sys.argv)==3 and sys.argv[1]=='--audit':audit_completed(Path(sys.argv[2]))
    elif len(sys.argv)==3 and sys.argv[1]=='--smoke':smoke(Path(sys.argv[2]))
    else:
        assert len(sys.argv)==1,'usage: test_ext4_mount_host.py [--audit|--smoke completed-host-directory]'
        main()
