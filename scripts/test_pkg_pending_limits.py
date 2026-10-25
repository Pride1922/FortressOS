"""Actual EXT4 block and inode exhaustion behind durable package credits."""
import argparse,datetime,json,pathlib,shutil,struct,subprocess
from pkg_enroll_fixture import command,gpt,BS
from pkg_enroll_records import H
ROOT=pathlib.Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser();parser.add_argument('evidence');args=parser.parse_args();prior=pathlib.Path(args.evidence).resolve();old=json.loads((prior/'manifest.json').read_text())
    out=ROOT/'.codex-remote-attachments'/('pkg-pending-limits-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'));out.mkdir();work=out/'workspace'
    sha=lambda p:H(p.read_bytes()).hex();guards={str(p.relative_to(ROOT)):sha(p) for f in ('src','user','scripts','tests') for p in (ROOT/f).rglob('*') if p.is_file() and '__pycache__' not in p.parts}
    guards.update({str(p.relative_to(ROOT)):sha(p) for p in [ROOT/'Makefile',ROOT/'AGENTS.md',ROOT/'PROTECTED.md',ROOT/'docs/plans/PKG2_PRODUCTION_INTEGRATION.md',*(ROOT/'bin').glob('*')] if p.is_file()})
    m={'runtime':'INCOMPLETE','preservation':'INCOMPLETE','boot_acceptance':'NOT RUN','commands':[],'cases':[],'guards':guards}
    def save():(out/'manifest.json').write_text(json.dumps(m,indent=2)+'\n')
    def run(cmd,label):
        m['commands'].append(cmd);save();r=subprocess.run(cmd,cwd=work,capture_output=True,text=True,timeout=600);(out/(label+'.log')).write_text(r.stdout+r.stderr);assert not r.returncode,(label,r.stdout[-1200:],r.stderr[-2000:]);return r.stdout
    try:
        shutil.copytree(prior/'workspace',work)
        # Keep the saved private EXT4 overlay, but compile the current recovery
        # implementation and tests rather than silently testing an old copy.
        for name in ('tests/pkg_pending_fs.inc','tests/pkg_pending_candidate.c','tests/pkg_pending_candidate.h','tests/pkg_writer_candidate.c','tests/pkg_writer_candidate.h','src/kernel/pkg_owner.c','src/kernel/pkg_owner.h'):
            shutil.copyfile(ROOT/name,work/name)
        shutil.copyfile(ROOT/'tests/pkg_pending_limits.inc',work/'tests/pkg_pending_limits.inc')
        host=(ROOT/'tests/pkg_pending_fs_host.c').read_text();host=host.replace('int main(int argc,char **argv){','#include "pkg_pending_limits.inc"\nint main(int argc,char **argv){')
        host=host.replace('assert(!r);assert(!native_recover(e));','assert(!r);limit_before(e,&d);assert(!native_recover(e));')
        host=host.replace('assert(pn_view.phase==PG_IDLE);native_close(e);','assert(pn_view.phase==PG_IDLE);limit_after(e,&d);native_close(e);')
        (work/'tests/pkg_pending_limits_generated.c').write_text(host)
        m['compiled_inputs']={str(p.relative_to(work)):sha(p) for f in ('src','user','scripts','tests') for p in (work/f).rglob('*') if p.is_file() and '__pycache__' not in p.parts}
        cmd=next(c for c in old['commands'] if 'tests/pkg_pending_fs_host.c' in c);native=out/'limits-native'
        cmd=[str(native) if x==str(prior/'pending-native') else 'tests/pkg_pending_limits_generated.c' if x=='tests/pkg_pending_fs_host.c' else x for x in cmd];run(cmd,'compile')
        empty=out/'empty';empty.write_bytes(b'')
        for label in ('reserved','pending-staged','retiring'):
            original=prior/label;directory=out/label;directory.mkdir();data=(original/'disk.img').read_bytes();root=directory/'filled.root';root.write_bytes(data[2048*512:(2048+65536)*512]);current=(original/'current').read_bytes();grant=current[96+2656+512:96+2656+512+1024];rb,ri=struct.unpack_from('<QQ',grant,312)
            fi=struct.unpack_from('<I',root.read_bytes(),1040)[0];count=fi-(ri+16)-1;assert count>0
            script=directory/'fill.commands';script.write_text(''.join(f'write {empty} /ordinary-{i}\n' for i in range(count)));run(['debugfs','-w','-f',str(script),str(root)],label+'-fill-inodes')
            fb=struct.unpack_from('<I',root.read_bytes(),1036)[0];padding=directory/'padding';padding.write_bytes(b'Z'*((fb-rb-64)*BS));run(['debugfs','-w','-R',f'write {padding} /ordinary-pad',str(root)],label+'-fill-blocks')
            filled=root.read_bytes();assert struct.unpack_from('<II',filled,1036)==(rb+64,ri+16)
            run(['e2fsck','-fn',str(root)],label+'-initial-fsck');disk=directory/'filled.img';disk.write_bytes(gpt(filled));recovered=directory/'recovered.img'
            run([str(native),str(disk),str(original/'capsule'),str(original/'selector'),str(original/'witness'),str(recovered),'-1','1'],label+'-runtime')
            root.write_bytes(recovered.read_bytes()[2048*512:(2048+65536)*512]);run(['e2fsck','-fn',str(root)],label+'-final-fsck');m['cases'].append({'label':label,'free_before':[rb+64,ri+16],'runtime':'PASS'});save()
        m['runtime']='PASS'
    except BaseException as e:m['runtime']='FAIL';m['error']=repr(e);raise
    finally:
        m['changed_inputs']=[p for p,h in guards.items() if sha(ROOT/p)!=h];m['changed_copies']=[p for p,h in m.get('compiled_inputs',{}).items() if sha(work/p)!=h]
        m['preservation']='FAIL' if m['changed_inputs'] or m['changed_copies'] else 'PASS';save();print(out,m['runtime'],m['preservation'],flush=True)
    assert m['preservation']=='PASS'
if __name__=='__main__':main()
