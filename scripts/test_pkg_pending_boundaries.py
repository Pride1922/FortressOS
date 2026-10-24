"""Native pending admission refusals and authenticated projected replay."""
import argparse,datetime,json,pathlib,re,shutil,struct,subprocess
from concurrent.futures import ThreadPoolExecutor,as_completed
from pkg_enroll_records import H,sealed
from pkg_enroll_fixture import gpt,BS
from pkg_boot_journal_fixture import pending
from test_pkg_pending_boot import recovery_boot
ROOT=pathlib.Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser();parser.add_argument('boot_evidence');parser.add_argument('--native',required=True);args=parser.parse_args();base=pathlib.Path(args.boot_evidence).resolve();native=pathlib.Path(args.native).resolve();work=base/'workspace';iso=base/'valid.iso'
    out=ROOT/'.codex-remote-attachments'/('pkg-pending-boundaries-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'));out.mkdir();sha=lambda p:H(p.read_bytes()).hex()
    guards={str(p.relative_to(ROOT)):sha(p) for folder in ('src','user','scripts','tests') for p in (ROOT/folder).rglob('*') if p.is_file() and '__pycache__' not in p.parts}
    guards.update({str(p.relative_to(ROOT)):sha(p) for p in [ROOT/'Makefile',ROOT/'AGENTS.md',ROOT/'PROTECTED.md',ROOT/'docs/plans/PKG2_PRODUCTION_INTEGRATION.md',*(ROOT/'bin').glob('*')] if p.is_file()})
    compiled={str(p.relative_to(work)):sha(p) for p in work.rglob('*') if p.is_file() and p.suffix in ('.c','.h','.inc','.asm')}
    m={'boot_runtime':'INCOMPLETE','preservation':'INCOMPLETE','commands':[],'cases':[],'guards':guards,'copied_sources':compiled}
    def save():(out/'manifest.json').write_text(json.dumps(m,indent=2)+'\n')
    def run(cmd,label):
        m['commands'].append(cmd);save();r=subprocess.run(cmd,cwd=work,capture_output=True,text=True,timeout=240);(out/(label+'.log')).write_text(r.stdout+r.stderr);assert not r.returncode,(label,r.stdout[-1200:],r.stderr[-2000:]);return r.stdout
    try:
        original=(base/'pending-staged.img').read_bytes();root=original[2048*512:(2048+65536)*512];witness=(base/'pending-staged.witness').read_bytes();jobs=[]
        def record(data,index,body=None):
            row=witness[32+128*index:32+128*(index+1)];size=struct.unpack_from('<Q',row,16)[0];at=0;result=b''
            if body is not None:assert len(body)==size
            for j in range(struct.unpack_from('<H',row,32)[0]):
                lo,n,physical=struct.unpack_from('<IIQ',row,40+16*j);take=min(size-at,n*BS)
                if body is None:result+=data[physical*BS:physical*BS+take]
                else:data[physical*BS:physical*BS+take]=body[at:at+take]
                at+=take
            assert at==size;return result
        for label in ('forged-charge','forged-total','premature-idle','unknown-transaction','activation-generation','activation-hardlink','wrong-next','below-block-reserve'):
            data=bytearray(root);path=out/(label+'.root')
            if label in ('forged-charge','forged-total'):
                grant=bytearray(record(data,1));table=bytearray(record(data,2));account=bytearray(record(data,5))
                if label=='forged-charge':
                    struct.pack_into('<Q',table,296,struct.unpack_from('<Q',table,296)[0]+1);table=sealed(table)
                    struct.pack_into('<Q',grant,312,struct.unpack_from('<Q',grant,312)[0]-1);struct.pack_into('<Q',grant,328,struct.unpack_from('<Q',grant,328)[0]+1);grant[264:296]=H(table);record(data,2,table)
                else:
                    for offset in (296,312):struct.pack_into('<Q',grant,offset,struct.unpack_from('<Q',grant,offset)[0]+1)
                grant=sealed(grant);account[448:480]=H(grant);record(data,1,grant);record(data,5,sealed(account))
            elif label=='premature-idle':
                bundle=(base/'policy.bin').read_bytes()[1152:];parts=[];at=96
                for i in range(8):n=struct.unpack_from('<I',bundle,32+8*i)[0];parts.append(bundle[at:at+n]);at+=n
                for part,index in ((parts[2],1),(parts[3],2),(parts[4],5),(parts[6],6)):record(data,index,part)
            path.write_bytes(data)
            if label in ('unknown-transaction','activation-generation','activation-hardlink','wrong-next','below-block-reserve'):
                if label=='unknown-transaction':
                    payload=out/'foreign';payload.write_bytes(b'foreign');run(['debugfs','-w','-R',f'write {payload} /pkg/transaction.v1/unowned',str(path)],label+'-prepare')
                elif label=='activation-generation':run(['debugfs','-w','-R','set_inode_field /pkg/transaction.v1/activation generation 9999',str(path)],label+'-prepare')
                elif label=='activation-hardlink':
                    run(['debugfs','-w','-R','ln /pkg/transaction.v1/activation /alias',str(path)],label+'-link');run(['debugfs','-w','-R','set_inode_field /pkg/transaction.v1/activation links_count 2',str(path)],label+'-links')
                elif label=='wrong-next':
                    from test_jbd2_replay_host import blocks
                    block=blocks(path,'/pkg/transaction.v1/next.v1')[0];data=bytearray(path.read_bytes());data[block*BS]^=1;path.write_bytes(data)
                else:
                    remaining=struct.unpack_from('<Q',record(data,1),312)[0];free=struct.unpack_from('<I',data,1036)[0];payload=out/'padding';payload.write_bytes(b'X'*((free-remaining-63)*BS));run(['debugfs','-w','-R',f'write {payload} /ordinary-pad',str(path)],label+'-prepare')
                    assert struct.unpack_from('<I',path.read_bytes(),1036)[0]==remaining+63
            target=out/(label+'.img');target.write_bytes(gpt(path.read_bytes()));jobs.append((label,target,True,None,None,'bios',1))
        grant_block=struct.unpack_from('<Q',witness,32+128+48)[0]
        source,replayed=pending(base/'pending-staged.img',out/'journal-valid',grant_block)
        invalid,_=pending(base/'pending-staged.img',out/'journal-invalid',grant_block,True)
        expected=out/'projected-expected.img';cmd=[str(native),str(source),str(base/'policy.bin'),str(base/'selector.bin'),str(base/'pending-staged.witness'),str(expected),'-1','1'];text=run(cmd,'projected-native-oracle');counter=re.search(r'PASS native recovery writes=(\d+) flushes=(\d+)',text);assert counter;counts=tuple(map(int,counter.groups()))
        jobs.append(('invalid-projected-pending',invalid,True,None,None,'bios',1))
        for fw,smp in (('bios',1),('bios',4),('uefi',1),('uefi',4)):jobs.append((f'projected-{fw}-{smp}',source,False,expected,counts,fw,smp))
        def execute(job):
            label,target,refused,expected,counts,fw,smp=job;values=[recovery_boot(out,work,iso,target,fw,smp,label,refused=refused,expected=expected,expected_counts=counts)]
            if not refused:values.append(recovery_boot(out,work,iso,out/label/'enrolled.img',fw,smp,label+'-reboot'))
            return values
        with ThreadPoolExecutor(max_workers=4) as pool:
            for future in as_completed([pool.submit(execute,j) for j in jobs]):m['cases'].extend(future.result());save()
        m['boot_runtime']='PASS'
    except BaseException as e:m['boot_runtime']='FAIL';m['error']=repr(e);raise
    finally:
        m['changed_inputs']=[p for p,h in guards.items() if sha(ROOT/p)!=h];m['changed_copies']=[p for p,h in compiled.items() if sha(work/p)!=h]
        m['preservation']='FAIL' if m['changed_inputs'] or m['changed_copies'] else 'PASS';save();print(out,m['boot_runtime'],m['preservation'],flush=True)
    assert m['preservation']=='PASS'
if __name__=='__main__':main()
