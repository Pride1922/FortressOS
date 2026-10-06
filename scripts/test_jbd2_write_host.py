"""Phase-7 actual writer + independent Linux replay on fresh regular files.
No mount commands, block devices or production write-policy changes.
"""
import hashlib,json,re,struct,tempfile
from pathlib import Path
from create_ext4_fixtures import ROOT,run
from test_jbd2_replay_host import FEATURES,blocks,be,put,seal,sbseal,oracle

def main(binary='jbd2_write_host',foundation=False,file_writes=False,minimum_journal=False,evidence_parent=None):
    parent=ROOT/(evidence_parent or ('build/ext4-phase8-2' if file_writes else 'build/ext4-phase8-1' if foundation else 'build/jbd2-write'));parent.mkdir(exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='minimum-journal-' if minimum_journal else 'run-',dir=parent));records=[]
    (out/'versions.txt').write_text(run(['mke2fs','-V'])+run(['debugfs','-V']))
    for bs in (1024,2048,4096):
        base=out/f'base-{bs}.img'
        with base.open('xb') as f:f.truncate(64*1024*1024)
        journal_mib=bs//1024 if minimum_journal else 4
        mkfs=['mke2fs','-q','-t','ext4','-b',str(bs),'-I','256','-O',FEATURES,'-E','lazy_itable_init=0','-m','0','-g','4096','-N','512','-J',f'size={journal_mib}',str(base)]
        log=run(mkfs);seed=out/f'seed-{bs}.bin';seed.write_bytes(b'O'*(bs*3))
        log+=run(['debugfs','-w','-R',f'write {seed} /target.bin',str(base)])
        fragmentino=0
        if file_writes:
            commands=out/f'fragment-{bs}.commands'
            capacity=(bs-12)//12
            commands.write_text('write /dev/null /fragment.bin\n'+''.join(
                f'fallocate /fragment.bin {k*2} {k*2}\n' for k in range(capacity*4+2))+
                f'set_inode_field /fragment.bin size {(capacity*8+4)*bs}\n')
            log+=run(['debugfs','-w','-f',str(commands),str(base)])
            fragmentino=int(re.search(r'Inode: (\d+)',run(['debugfs','-R','stat /fragment.bin',str(base)]))[1])
            log+=run(['e2fsck','-fn',str(base)])
        commands=out/f'initialize-{bs}.commands';commands.write_text('journal_open -c -v 3\njournal_close\n')
        log+=run(['debugfs','-w','-f',str(commands),str(base)])
        d=bytearray(base.read_bytes());ino=struct.unpack_from('<I',d,1248)[0]
        jmap=blocks(base,f'<{ino}>');targets=blocks(base,'/target.bin');assert len(targets)==3
        if minimum_journal:assert len(jmap)==1024
        stat=run(['debugfs','-R','stat /target.bin',str(base)]);fileino=int(re.search(r'Inode: (\d+)',stat)[1])
        source_mode=re.search(r'Mode:\s+(0[0-7]+)',stat)[1]
        js=bytearray(d[jmap[0]*bs:(jmap[0]+1)*bs]);assert be(js,40) in (0x10,0x11)
        # debugfs journal_open activates even an empty log. Explicitly create
        # the validated empty-journal boundary used by the writer workbench.
        put(js,28,0);put(js,88,1);put(js,40,0x11)
        b=bytearray(js[:1024]);seal(b,252,0xffffffff);js[:1024]=b
        d[jmap[0]*bs:(jmap[0]+1)*bs]=js
        # Phase 8 owns this transition. The disposable fixture is deliberately
        # recovery-needed so independent Linux replay examines written logs.
        struct.pack_into('<I',d,1120,struct.unpack_from('<I',d,1120)[0]|4);sbseal(d)
        config=out/f'config-{bs}.bin';config.write_bytes(struct.pack('<IIIII',bs,fileino,*targets)+
            (struct.pack('<I',fragmentino) if file_writes else b''))
        (out/f'generation-{bs}.log').write_text(log+stat)
        for wrap in (False,True):
            source=out/f'{bs}-{"wrap" if wrap else "normal"}.img';initial=bytearray(d)
            if wrap:
                put(js,24,0xffffffff);put(js,88,len(jmap)-2)
                b=bytearray(js[:1024]);seal(b,252,0xffffffff);js[:1024]=b
                initial[jmap[0]*bs:(jmap[0]+1)*bs]=js
            source.write_bytes(initial)
            for ss in (512,4096):
                prefix=out/f'{source.stem}-{ss}'
                committed=Path(str(prefix)+'-committed.img')
                checkpointed=Path(str(prefix)+'-checkpointed.img')
                recovered=Path(str(prefix)+'-recovered.img')
                cmd=[str(ROOT/'build'/binary),str(source),str(config),str(ss),str(committed),str(checkpointed),str(recovered)]
                text=run(cmd,timeout=600 if file_writes else 120);print(text,end='',flush=True)
                expected=(b'O'*(bs*3)+bytes(bs*2+23)+b'J'*33) if file_writes else bytes.fromhex('c03b3998')+b'A'*(bs-4)+b'C'*bs+b'O'*bs
                # Linux independently validates the writer's descriptor/tags,
                # escaped payload CRC, revoke and commit -- no normalization.
                for image in ((checkpointed,recovered) if foundation and not file_writes else (committed,checkpointed,recovered)):
                    linux=Path(str(image)+'.linux.img');oracle(image,linux,Path(str(image)+'.linux.log'))
                    dumped=Path(str(image)+'.bin');run(['debugfs','-R',f'dump /target.bin {dumped}',str(linux)])
                    assert dumped.read_bytes()==expected,(image,'bytes')
                    stat=run(['debugfs','-R','stat /target.bin',str(linux)])
                    assert re.search(r'Mode:\s+'+(source_mode if file_writes else '0600'),stat),stat
                if file_writes:
                    fragment=Path(str(checkpointed)+'.fragment.img');linux=Path(str(fragment)+'.linux.img')
                    oracle(fragment,linux,Path(str(fragment)+'.linux.log'))
                    dumped=Path(str(fragment)+'.bin');run(['debugfs','-R',f'dump /fragment.bin {dumped}',str(linux)])
                    expected_fragment=bytearray((capacity*8+4)*bs);expected_fragment[2*bs:3*bs]=b'H'*bs
                    assert dumped.read_bytes()==expected_fragment
                Path(str(prefix)+'.log').write_text(text)
                records.append({'block':bs,'sector':ss,'wrap':wrap,'argv':cmd,'journal_blocks':len(jmap),
                    'journal_mib':journal_mib,'minimum_journal':minimum_journal,
                    'committed_sha256':hashlib.sha256(committed.read_bytes()).hexdigest()})
    (out/'manifest.json').write_text(json.dumps(records,indent=2)+'\n')
    print(f'{"EXT4 journal file writes" if file_writes else "EXT4 transaction foundation" if foundation else "JBD2 writer"} host PASS 12/12; evidence: {out}; no journaled VFS/physical durability claim.')
if __name__=='__main__':main()
