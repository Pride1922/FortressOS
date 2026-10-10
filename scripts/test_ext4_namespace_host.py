"""Phase 8.3 actual namespace transactions; disposable regular images only."""
import hashlib,json,re,struct,tempfile
from pathlib import Path
from create_ext4_fixtures import ROOT,run
from test_jbd2_replay_host import FEATURES,blocks,be,put,seal,sbseal,oracle

def main():
    parent=ROOT/'build/ext4-phase8-3';parent.mkdir(exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='run-',dir=parent));records=[]
    (out/'versions.txt').write_text(run(['mke2fs','-V'])+run(['debugfs','-V']))
    for bs in (1024,2048,4096):
        base=out/f'base-{bs}.img'
        with base.open('xb') as f:f.truncate(64*1024*1024)
        log=run(['mke2fs','-q','-t','ext4','-b',str(bs),'-I','256','-O',FEATURES,
            '-E','lazy_itable_init=0','-m','0','-g','4096','-N','1024','-J','size=4',str(base)])
        seed=out/f'seed-{bs}.bin';seed.write_bytes(b'O'*(3*bs))
        commands=out/f'namespace-{bs}.commands'
        commands.write_text('mkdir /source\nmkdir /dest\nmkdir /empty\nmkdir /nonempty\nmkdir /full\n'+
            f'write {seed} /source/file.bin\nwrite {seed} /victim.bin\nwrite /dev/null /nonempty/child\n'+
            ''.join(f'write /dev/null /full/{k:03d}'+('f'*60)+'\n' for k in range((bs-12-24)//72))+
            'write /dev/null /tree.bin\n'+''.join(f'fallocate /tree.bin {k*2} {k*2}\n' for k in range(5))+
            f'set_inode_field /tree.bin size {9*bs}\nwrite /dev/null /large.bin\nfallocate /large.bin 0 64\n'+
            f'set_inode_field /large.bin size {65*bs}\njournal_open -c -v 3\njournal_close\n')
        log+=run(['debugfs','-w','-f',str(commands),str(base)])
        log+=run(['e2fsck','-fn',str(base)])
        names=('/source','/dest','/empty','/nonempty','/full','/source/file.bin','/victim.bin','/tree.bin','/large.bin')
        inos=[]
        for name in names:
            stat=run(['debugfs','-R',f'stat {name}',str(base)]);log+=stat
            inos.append(int(re.search(r'Inode: (\d+)',stat)[1]))
        d=bytearray(base.read_bytes());journalino=struct.unpack_from('<I',d,1248)[0]
        jmap=blocks(base,f'<{journalino}>');js=bytearray(d[jmap[0]*bs:(jmap[0]+1)*bs])
        assert be(js,40) in (0x10,0x11)
        put(js,28,0);put(js,88,1);put(js,40,0x11)
        b=bytearray(js[:1024]);seal(b,252,0xffffffff);js[:1024]=b
        struct.pack_into('<I',d,1120,0x46);sbseal(d)
        config=out/f'config-{bs}.bin';config.write_bytes(struct.pack('<10I',bs,*inos))
        (out/f'generation-{bs}.log').write_text(log)
        for wrap in (False,True):
            source=out/f'{bs}-{"wrap" if wrap else "normal"}.img'
            if wrap:
                put(js,24,0xffffffff);put(js,88,len(jmap)-2)
                b=bytearray(js[:1024]);seal(b,252,0xffffffff);js[:1024]=b
            d[jmap[0]*bs:(jmap[0]+1)*bs]=js;source.write_bytes(d)
            for ss in (512,4096):
                prefix=out/f'{source.stem}-{ss}'
                cmd=[str(ROOT/'build/ext4_namespace_host'),str(source),str(config),str(ss),str(prefix)]
                text=run(cmd,timeout=600);Path(str(prefix)+'.log').write_text(text);print(text,end='',flush=True)
                for profile in range(9):
                    for phase in ('committed','checkpointed','recovered'):
                        image=Path(f'{prefix}-{profile}-{phase}.img');linux=Path(str(image)+'.linux.img')
                        oracle(image,linux,Path(str(image)+'.linux.log'))
                        operation_path=('/newfile','/newdir','/source/renamed.bin','/dest/renamed.bin',
                            '/victim.bin','/empty','/full/'+('n'*63),'/full/'+('n'*63),'/tree.bin')[profile]
                        stat=run(['debugfs','-R',f'stat {operation_path}',str(linux)])
                        assert ('File not found' in stat)==(profile in (4,5,8)),stat
                        if profile in (2,3,7):
                            assert 'File not found' in run(['debugfs','-R','stat /source/file.bin',str(linux)])
                            dumped=Path(str(image)+'.bin');run(['debugfs','-R',f'dump {operation_path} {dumped}',str(linux)])
                            assert dumped.read_bytes()==seed.read_bytes()
                        if profile in (0,6):assert 'Size: 0' in stat,stat
                        if profile==1:
                            listing=run(['debugfs','-R','ls -l /newdir',str(linux)])
                            assert '..' in listing and re.search(r'Mode:\s+0755',stat),listing+stat
                records.append({'block':bs,'sector':ss,'wrap':wrap,'argv':cmd,
                    'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest()})
    (out/'manifest.json').write_text(json.dumps(records,indent=2)+'\n')
    print(f'EXT4 namespace host PASS 12/12, 324 Linux oracle copies; evidence: {out}; no VFS/physical journal claim.')

if __name__=='__main__':main()
