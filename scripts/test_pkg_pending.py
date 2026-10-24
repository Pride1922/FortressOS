"""Combined recovery/escrow actual-code gate, isolated copied sources only."""
import datetime,hashlib,json,pathlib,shutil,struct,subprocess,tempfile
from pkg_enroll_fixture import create,gpt,command
from pkg_enroll_records import H,write_fresh_records
from pkg_pending_records import prepare
from pkg_boot_admission_overlay import overlay
from test_pkg_service_integration import function
ROOT=pathlib.Path(__file__).resolve().parents[1]

def main():
    out=ROOT/'.codex-remote-attachments'/('pkg-pending-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'));out.mkdir();work=out/'workspace'
    sha=lambda p:H(p.read_bytes()).hex()
    guards={str(p.relative_to(ROOT)):sha(p) for name in ('src','user','scripts','tests') for p in (ROOT/name).rglob('*') if p.is_file() and '__pycache__' not in p.parts}
    guards.update({str(p.relative_to(ROOT)):sha(p) for p in [ROOT/'Makefile',ROOT/'AGENTS.md',ROOT/'PROTECTED.md',ROOT/'docs/plans/PKG2_PRODUCTION_INTEGRATION.md',*(ROOT/'bin').glob('*')] if p.is_file()})
    m={'runtime':'INCOMPLETE','preservation':'INCOMPLETE','boot_acceptance':'NOT RUN','commands':[],'cases':[],'guards':guards}
    def save():(out/'manifest.json').write_text(json.dumps(m,indent=2)+'\n')
    def run(cmd,label):
        m['commands'].append(cmd);save();p=subprocess.run(cmd,cwd=work,capture_output=True,text=True,timeout=600);(out/(label+'.log')).write_text(p.stdout+p.stderr)
        assert not p.returncode,(label,p.returncode,p.stdout[-1200:],p.stderr[-2500:])
    def replace(name,before,after):
        p=work/name;s=p.read_text();assert s.count(before)==1,(name,before,s.count(before));p.write_text(s.replace(before,after))
    try:
        shutil.copytree(ROOT,work,ignore=shutil.ignore_patterns('.git','.codex-remote-attachments','build','bin','dist','__pycache__','*.iso','*.img'))
        overlay(work,replace)
        replace('src/fs/ext4.c','\n#include "../../tests/pkg_boot_admission.inc"\n','\n')
        replace('src/fs/ext4_transaction.inc','static int e4_semantic_commit(ext4_engine_t *e) {','static int pn_core_commit(ext4_engine_t *e) {')
        replace('src/fs/ext4.c','#include "ext4_mutate.inc"','struct ext4_engine;\nstatic int e4_semantic_commit(struct ext4_engine *);\n#include "ext4_mutate.inc"')
        export=function((work/'tests/pkg_boot_read_generated.inc').read_text(),'static int export_bundle(').replace('static int export_bundle(','static int pn_export_bundle(')
        (work/'tests/pkg_pending_export_generated.inc').write_text(export+'\n')
        m['compiled_inputs']={str(p.relative_to(work)):sha(p) for folder in ('src','user','tests','scripts') for p in (work/folder).rglob('*') if p.is_file() and '__pycache__' not in p.parts}
        flags=['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-pthread','-DTOOL_HOST_TEST','-DFORTRESS_DAC_ENFORCED','-I.','-Itests','-Itests/ext4_host','-Itests/host','-Isrc/include','-Isrc/fs','-Isrc/drivers','-Isrc/mm','-Isrc/lib','-Isrc/kernel','-Iuser/tools','-Isrc/lib/crypto/upstream/src']
        pure=['tests/pkg_pending_candidate.c','tests/pkg_current_candidate.c','tests/pkg_selector_candidate.c','tests/pkg_enrollment_candidate.c','tests/pkg_grant_candidate.c','tests/pkg_mount_candidate.c','user/pkg/catalog.c','user/pkg/verify.c','user/pkg/format.c','user/pkg/manifest.c','user/tools/gzip.c','user/tools/common.c','user/tools/digest.c','src/lib/crypto/hash.c','src/lib/crypto/curve.c']
        objs=[]
        for i,src in enumerate(('src/lib/crypto/upstream/src/monocypher.c','src/lib/crypto/upstream/src/optional/monocypher-ed25519.c')):
            p=out/f'mono{i}.o';run(flags+['-include','src/lib/crypto/namespace.h','-c',src,'-o',str(p)],f'crypto-{i}');objs.append(str(p))
        reader=out/'pending-reader';native=out/'pending-native'
        run(flags+pure+['tests/pkg_pending_host.c',*objs,'-o',str(reader)],'compile-pure')
        run(flags+pure+['tests/pkg_pending_fs_host.c','tests/ext4_fault_disk.c','src/fs/permission_values.c','src/kernel/creds.c','src/lib/account_redo.c',*objs,'-o',str(native)],'compile-native')
        owner=out/'recovery-owner';owner_flags=['gcc','-std=c11','-O1','-g','-fsanitize=address,undefined','-Wall','-Wextra','-Werror','-no-pie','-pthread','-D_XOPEN_SOURCE=700','-DTEST_SMP_MEMORY','-Itests/ext4_host','-Itests','-Isrc/include','-Isrc/kernel']
        run(owner_flags+['tests/pkg_recovery_owner_host.c','tests/pkg_writer_candidate.c','src/kernel/process_table.c','src/kernel/creds.c','-o',str(owner)],'compile-owner');run([str(owner)],'owner-runtime')
        run(['gcc','-std=c11','-Os','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-mno-red-zone','-mno-sse','-mno-mmx','-msoft-float','-fstack-usage','-Wframe-larger-than=1024','-I.','-Itests','-Isrc/include','-Isrc/fs','-Iuser/tools','-c','tests/pkg_pending_candidate.c','-o',str(out/'pending-free.o')],'freestanding')
        with tempfile.TemporaryDirectory(prefix='fortress-pending-') as t:
            t=pathlib.Path(t);roles={}
            for name,values in (('selector',(111,112,113)),('enrollment',(41,42,43)),('catalog',(1,2,3))):
                roles[name]=[]
                for v in values:p=t/f'{name}-{v}';p.write_bytes(bytes([v])*32);p.chmod(0o600);roles[name].append(p)
            for label,phase,published,installed in (('reserved',1,False,False),('pending-staged',2,False,False),('pending-published',2,True,False),('pending-state',2,True,True),('retiring',3,True,True)):
                fixture=t/label;fixture.mkdir();image,_=create(fixture,identities=True)
                prepared=prepare(image,fixture/'prepared',roles,H(b'HOST TEST kernel'),H(b'HOST TEST bootstrap'),phase,published,installed);write_fresh_records(image,prepared)
                directory=out/label;directory.mkdir();data=image.read_bytes();free=struct.unpack_from('<II',data,1036);capacity=(struct.unpack_from('<I',data,1028)[0],struct.unpack_from('<I',data,1024)[0])
                for name,value in (('policy',prepared['policy'][:1136]),('capsule',prepared['policy']),('selector',prepared['selector']),('baseline',prepared['baseline']),('current',prepared['bundle']),('artifact',prepared['artifact']),('intent',prepared['intent']),('objects',prepared['objects']),('witness',prepared['witness']),('budget',struct.pack('<4Q',*free,*capacity)),('disk.img',gpt(data))):(directory/name).write_bytes(value)
                argv=[str(directory/p) for p in ('policy','selector','baseline','current','artifact','intent','objects')]+[str(int(published)),str(directory/'budget')]
                run([str(reader),*argv,'1'],label+'-model')
                for name in ('objects','artifact','intent'):
                    path=directory/name;original=path.read_bytes()
                    if not original:continue
                    bad=bytearray(original);bad[8 if name=='objects' else -1]^=1;path.write_bytes(bad)
                    run([str(reader),*argv,'0'],label+'-bad-'+name);path.write_bytes(original)
                objects=directory/'objects';original=objects.read_bytes()
                for offset in (0,4,12,16,20,24,28,32,40,48,80,84,88):
                    bad=bytearray(original);bad[offset]^=1;objects.write_bytes(bad);run([str(reader),*argv,'0'],label+'-bad-object-'+str(offset));objects.write_bytes(original)
                budget=directory/'budget';original=budget.read_bytes();budget.write_bytes(struct.pack('<4Q',63,15,*capacity));run([str(reader),*argv,'0'],label+'-below-account-reserve');budget.write_bytes(original)
                run([str(native),str(directory/'disk.img'),str(directory/'capsule'),str(directory/'selector'),str(directory/'witness'),str(directory/'recovered.img'),'40','1'],label+'-native')
                final=(directory/'recovered.img').read_bytes();assert final[:2048*512]==gpt(data)[:2048*512] and final[(2048+65536)*512:]==gpt(data)[(2048+65536)*512:]
                root=directory/'root-after.img';root.write_bytes(final[2048*512:(2048+65536)*512]);run(['e2fsck','-fn',str(root)],label+'-linux-fsck')
                m['cases'].append({'label':label,'model':'PASS','native':'PASS','linux':'PASS'});save()
            m['runtime']='PASS'
    except BaseException as e:m['runtime']='FAIL';m['error']=repr(e);raise
    finally:
        m['changed_inputs']=[p for p,h in guards.items() if sha(ROOT/p)!=h]
        m['changed_copies']=[p for p,h in m.get('compiled_inputs',{}).items() if sha(work/p)!=h]
        m['preservation']='FAIL' if m['changed_inputs'] or m['changed_copies'] else 'PASS';save();print(out,m['runtime'],m['preservation'],flush=True)
    assert m['preservation']=='PASS'
if __name__=='__main__':main()
