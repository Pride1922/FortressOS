"""Phase 8.4 restartable traditional orphan workbench; regular fixtures only."""
import argparse,hashlib,json,re,struct,tempfile,subprocess
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
from create_ext4_fixtures import ROOT,run
from test_jbd2_replay_host import FEATURES,blocks,be,put,seal,sbseal,oracle

def verify(task):
    bs,ss,wrap,source,config,prefix,last=task
    cmd=[str(ROOT/'build/ext4_orphan_host'),str(source),str(config),str(ss),str(prefix)]
    logfile=Path(str(prefix)+'.log')
    first=0
    completed=logfile.is_file() and re.search(rf'EXT4 orphan host PASS block={bs} sector={ss} cuts=\d+;',logfile.read_text())
    if not completed:
        first=cuts=0
        if logfile.is_file() and 'orphan recovery PASS' in logfile.read_text():
            for profile in range(7):
                match=re.search(rf'orphan profile={profile} PASS events=(\d+)',logfile.read_text())
                if not match:break
                assert all(Path(f'{prefix}-{profile}-{phase}.img').is_file() for phase in ('pending','complete','recovered'))
                first+=1;cuts+=int(match[1])*8
        if first:cmd.extend((str(first),str(cuts)))
        # The C harness creates evidence exclusively. Preserve incomplete
        # outputs from an interrupted profile before retrying that profile.
        leftovers=(list(prefix.parent.glob(f'{prefix.name}-*.img*')) if not first else
            [p for profile in range(first,7) for p in prefix.parent.glob(f'{prefix.name}-{profile}-*.img*')])
        if leftovers:
            archive=Path(tempfile.mkdtemp(prefix='interrupted-',dir=prefix.parent))
            for path in leftovers:path.rename(archive/path.name)
        with logfile.open('a' if first else 'w') as stream:
            result=subprocess.run(cmd,stdout=stream,stderr=subprocess.STDOUT,timeout=7200)
        assert result.returncode==0,f'{cmd}: exit {result.returncode}; retained log: {logfile}'
    text=logfile.read_text();print(text,end='',flush=True)
    for profile in range(7):
        for phase in ('pending','complete','recovered'):
            image=Path(f'{prefix}-{profile}-{phase}.img');linux=Path(str(image)+'.linux.img')
            oracle(image,linux,Path(str(image)+'.linux.log'))
            path=('/victim.bin','/large.bin','/large.bin','/tree.bin','/empty','/victim.bin','/deep.bin')[profile]
            stat=run(['debugfs','-R',f'stat {path}',str(linux)])
            assert ('File not found' in stat)==(profile in (2,3,4,5)),stat
            if profile in (0,1,6):
                expected=last*bs if profile==6 else bs+17
                assert re.search(rf'Size: {expected}\b',stat),stat
            if profile==0:
                dumped=Path(str(image)+'.bin');run(['debugfs','-R',f'dump {path} {dumped}',str(linux)])
                assert dumped.read_bytes()==b'O'*(bs+17)
    for extra in ('credit-recovered','foreign-recovered','lifetime-reuse'):
        image=Path(f'{prefix}-{extra}.img')
        oracle(image,Path(str(image)+'.linux.img'),Path(str(image)+'.linux.log'))
        if extra=='lifetime-reuse':
            dumped=Path(str(image)+'.bin')
            run(['debugfs','-R',f'dump /reuse {dumped}',str(image)+'.linux.img'])
            assert dumped.read_bytes()==b'N'*bs
    return {'block':bs,'sector':ss,'wrap':wrap,'argv':cmd,
        'retained_completed_execution':bool(completed),'resumed_profiles':first,
        'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest()}

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--resume',type=Path);args=parser.parse_args()
    parent=ROOT/'build/ext4-phase8-4';parent.mkdir(exist_ok=True)
    if args.resume:
        out=args.resume.resolve();assert out.is_dir() and out.is_relative_to(parent.resolve())
        tasks=[]
        for bs in (1024,2048,4096):
            config=out/f'config-{bs}.bin';assert config.is_file() and config.stat().st_size==28
            last=struct.unpack('<7I',config.read_bytes())[-1]
            for wrap in (False,True):
                source=out/f'{bs}-{"wrap" if wrap else "normal"}.img'
                assert source.is_file() and source.stat().st_size==32*1024*1024
                for ss in (512,4096):
                    prefix=out/f'{bs}-{"wrap" if wrap else "normal"}-{ss}'
                    tasks.append((bs,ss,wrap,source,config,prefix,last))
        with ThreadPoolExecutor(max_workers=3) as pool:records=list(pool.map(verify,tasks))
        (out/'manifest.json').write_text(json.dumps(records,indent=2)+'\n')
        print(f'EXT4 orphan host PASS 12/12, 288 Linux oracle copies; evidence: {out}; no VFS/physical journal claim.')
        return
    out=Path(tempfile.mkdtemp(prefix='run-',dir=parent));tasks=[]
    (out/'versions.txt').write_text(run(['mke2fs','-V'])+run(['debugfs','-V']))
    for bs in (1024,2048,4096):
        base=out/f'base-{bs}.img'
        with base.open('xb') as f:f.truncate(32*1024*1024)
        log=run(['mke2fs','-q','-t','ext4','-b',str(bs),'-I','256','-O',FEATURES,
            '-E','lazy_itable_init=0','-m','0','-g','4096','-N','512','-J','size=4',str(base)])
        seed=out/f'seed-{bs}.bin';seed.write_bytes(b'O'*(3*bs))
        capacity=(bs-12)//12;count=4*capacity+1;last=2*(count-1)
        commands=out/f'orphan-{bs}.commands'
        commands.write_text(f'write {seed} /victim.bin\nwrite /dev/null /large.bin\nfallocate /large.bin 0 64\n'+
            f'set_inode_field /large.bin size {65*bs}\nmkdir /empty\nwrite /dev/null /tree.bin\n'+
            ''.join(f'fallocate /tree.bin {k*2} {k*2}\n' for k in range(5))+f'set_inode_field /tree.bin size {9*bs}\n'+
            'write /dev/null /deep.bin\n'+''.join(f'fallocate /deep.bin {k*2} {k*2}\n' for k in range(count))+
            f'set_inode_field /deep.bin size {(last+1)*bs}\njournal_open -c -v 3\njournal_close\n')
        log+=run(['debugfs','-w','-f',str(commands),str(base)],timeout=300)
        log+=run(['e2fsck','-fn',str(base)])
        inos=[]
        for name in ('/victim.bin','/large.bin','/tree.bin','/empty','/deep.bin'):
            stat=run(['debugfs','-R',f'stat {name}',str(base)]);log+=stat
            inos.append(int(re.search(r'Inode: (\d+)',stat)[1]))
        assert '(ETB0)' in stat and '(ETB1)' in stat,stat
        d=bytearray(base.read_bytes());journalino=struct.unpack_from('<I',d,1248)[0]
        jmap=blocks(base,f'<{journalino}>');js=bytearray(d[jmap[0]*bs:(jmap[0]+1)*bs])
        assert be(js,40) in (0x10,0x11)
        put(js,28,0);put(js,88,1);put(js,40,0x11)
        b=bytearray(js[:1024]);seal(b,252,0xffffffff);js[:1024]=b
        struct.pack_into('<I',d,1120,0x46);sbseal(d)
        config=out/f'config-{bs}.bin';config.write_bytes(struct.pack('<7I',bs,*inos,last))
        (out/f'generation-{bs}.log').write_text(log)
        for wrap in (False,True):
            source=out/f'{bs}-{"wrap" if wrap else "normal"}.img'
            if wrap:
                put(js,24,0xffffffff);put(js,88,len(jmap)-2)
                b=bytearray(js[:1024]);seal(b,252,0xffffffff);js[:1024]=b
            d[jmap[0]*bs:(jmap[0]+1)*bs]=js;source.write_bytes(d)
            for ss in (512,4096):
                prefix=out/f'{source.stem}-{ss}'
                tasks.append((bs,ss,wrap,source,config,prefix,last))
    # Three independent host processes; immutable inputs and separate output
    # prefixes. Never share a writable fixture or a mounted filesystem.
    with ThreadPoolExecutor(max_workers=3) as pool:records=list(pool.map(verify,tasks))
    (out/'manifest.json').write_text(json.dumps(records,indent=2)+'\n')
    print(f'EXT4 orphan host PASS 12/12, 288 Linux oracle copies; evidence: {out}; no VFS/physical journal claim.')

if __name__=='__main__':main()
