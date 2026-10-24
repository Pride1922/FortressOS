"""Actual-code host current-state proof against an immutable signed baseline."""
import datetime,hashlib,json,os,pathlib,shutil,struct,subprocess,tempfile
from pkg_enroll_fixture import create,gpt
from pkg_enroll_records import build,write_fresh_records,sealed
from pkg_current_records import payloads,advance
ROOT=pathlib.Path(__file__).resolve().parents[1]
H=lambda b:hashlib.sha256(b).digest()

def main():
    out=ROOT/'.codex-remote-attachments'/('pkg-current-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'));out.mkdir();work=out/'workspace';work.mkdir()
    sha=lambda p:H(p.read_bytes()).hex()
    guards={str(p.relative_to(ROOT)):sha(p) for name in ('src','user','scripts','tests') for p in (ROOT/name).rglob('*') if p.is_file() and '__pycache__' not in p.parts}
    guards.update({str(p.relative_to(ROOT)):sha(p) for p in [ROOT/'Makefile',ROOT/'PROTECTED.md',ROOT/'AGENTS.md',ROOT/'docs/plans/PKG2_PRODUCTION_INTEGRATION.md',*(ROOT/'bin').glob('*')] if p.is_file()})
    m={'runtime':'INCOMPLETE','preservation':'INCOMPLETE','commands':[],'cases':[],'guards':guards,'boot_acceptance':'NOT RUN'}
    def save():(out/'manifest.json').write_text(json.dumps(m,indent=2)+'\n')
    def run(cmd,label,cwd=work):
        m['commands'].append(cmd);save();p=subprocess.run(cmd,cwd=cwd,capture_output=True,text=True,timeout=240)
        (out/(label+'.log')).write_text(p.stdout+p.stderr)
        if p.returncode:raise RuntimeError((label,p.returncode,p.stderr[-2400:],p.stdout[-1000:]))
    flags=['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-pthread','-DTOOL_HOST_TEST','-DFORTRESS_DAC_ENFORCED','-I.','-Itests','-Itests/ext4_host','-Itests/host','-Isrc/include','-Isrc/fs','-Isrc/drivers','-Isrc/mm','-Isrc/lib','-Isrc/kernel','-Iuser/tools','-Isrc/lib/crypto/upstream/src']
    pure=['tests/pkg_current_candidate.c','tests/pkg_current_host.c','tests/pkg_selector_candidate.c','tests/pkg_enrollment_candidate.c','tests/pkg_grant_candidate.c','tests/pkg_mount_candidate.c','user/pkg/catalog.c','user/pkg/verify.c','user/pkg/format.c','user/pkg/manifest.c','user/tools/gzip.c','user/tools/common.c','user/tools/digest.c','src/lib/crypto/hash.c','src/lib/crypto/curve.c']
    native=['tests/pkg_current_fs_host.c','tests/ext4_fault_disk.c','src/fs/permission_values.c','src/kernel/creds.c','src/lib/account_redo.c','user/tools/digest.c']
    mono=['src/lib/crypto/upstream/src/monocypher.c','src/lib/crypto/upstream/src/optional/monocypher-ed25519.c']
    try:
        text=''
        for label,cmd in (('dependencies',flags+['-MM',*sorted(set(pure+native))]),('crypto-dependencies',flags+['-include','src/lib/crypto/namespace.h','-MM',*mono])):
            p=subprocess.run(cmd,cwd=ROOT,capture_output=True,text=True,timeout=60);(out/(label+'.log')).write_text(p.stdout+p.stderr);assert not p.returncode,p.stderr;text+=p.stdout;m['commands'].append(cmd)
        names={os.path.normpath(p) for p in text.replace('\\\n',' ').split() if not p.endswith(':')}
        for name in names:
            source=ROOT/name;assert source.resolve().is_relative_to(ROOT);dest=work/name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,dest)
        m['compiled_inputs']={name:sha(work/name) for name in sorted(names)}
        objects=[]
        for i,src in enumerate(mono):
            obj=out/f'mono{i}.o';run(flags+['-include','src/lib/crypto/namespace.h','-c',src,'-o',str(obj)],f'compile-crypto-{i}');objects.append(str(obj))
        verifier=out/'current-reader';reader=out/'native-reader'
        run(flags+pure+objects+['-o',str(verifier)],'compile-current');run(flags+native+['-o',str(reader)],'compile-native')
        run(['gcc','-std=c11','-Os','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-mno-red-zone','-mno-sse','-mno-mmx','-msoft-float','-fstack-usage','-Wframe-larger-than=1024','-I.','-Itests','-Isrc/include','-Isrc/fs','-Iuser/tools','-c','tests/pkg_current_candidate.c','-o',str(out/'current-free.o')],'freestanding')
        with tempfile.TemporaryDirectory(prefix='fortress-current-') as temp:
            temp=pathlib.Path(temp);roles={}
            for name,values in (('selector',(111,112,113)),('enrollment',(41,42,43)),('catalog',(1,2,3))):
                roles[name]=[]
                for v in values:p=temp/f'{name}-{v}';p.write_bytes(bytes([v])*32);p.chmod(0o600);roles[name].append(p)
            directory=temp/'fixture';directory.mkdir();image,_=create(directory,identities=True)
            artifact,exe=payloads(image,temp/'payloads');baseline=build(image,temp/'records',roles,H(b'HOST TEST kernel'),H(b'HOST TEST bootstrap'),artifact=artifact)
            parts,current=advance(baseline,artifact,exe);prepared=dict(baseline,parts=parts);write_fresh_records(image,prepared)
            for name,data in (('policy',baseline['policy']),('selector',baseline['selector']),('baseline',baseline['bundle']),('current',current),('artifact',artifact),('executable',exe),('witness',baseline['witness']),('empty',b'')):(out/name).write_bytes(data)
            count=0
            def case(label,cur=current,a=artifact,e=exe,floor=2,ok=False,base=baseline['bundle'],policy=baseline['policy'],selector=baseline['selector']):
                nonlocal count
                count+=1;prefix=out/f'vector-{count:03d}';paths=[]
                for suffix,data in (('policy',policy),('selector',selector),('baseline',base),('current',cur),('artifact',a),('executable',e)):
                    p=pathlib.Path(str(prefix)+'.'+suffix);p.write_bytes(data);paths.append(str(p))
                run([str(verifier),*paths,str(floor),str(floor),'1' if ok else '0'],f'vector-{count:03d}')
                m['cases'].append({'label':label,'accepted':ok})
            case('authorized-current',ok=True);case('current-above-floor',floor=1,ok=True)
            case('genesis-at-floor-1',cur=baseline['bundle'],a=b'',e=b'',floor=1,ok=True)
            case('rollback-to-genesis',cur=baseline['bundle'],a=b'',e=b'')
            case('current-is-not-baseline',base=current)
            case('missing-artifact',a=b'');case('missing-executable',e=b'')
            for label,data,field in (('artifact',artifact,'a'),('executable',exe,'e')):
                bad=bytearray(data);bad[-1]^=1;case('corrupt-'+label,**{field:bytes(bad)})
            for floor in (0,3,2**64-1):case('unsupported-floor-'+str(floor),floor=floor)
            for i in range(8):
                changed=list(parts);bad=bytearray(changed[i]);bad[0]^=1;changed[i]=bytes(bad);case('mixed-record-'+str(i),cur=current[:96]+b''.join(changed))
            def changed_case(label,index,offset,value,actual_executable=exe):
                changed=list(parts);bad=bytearray(changed[index]);bad[offset:offset+len(value)]=value;changed[index]=sealed(bad)
                # Rebind all dependent mutable hashes: refusals must come from
                # authenticated lineage/policy, not an easily stale checksum.
                if index==3:
                    g=bytearray(changed[2]);g[264:296]=H(changed[3]);changed[2]=sealed(g)
                if index==6:
                    g=bytearray(changed[2]);g[168:232]=H(changed[6])*2;changed[2]=sealed(g)
                if index in (2,3,6):
                    account=bytearray(changed[4]);account[448:480]=H(changed[2]);account[512:544]=H(changed[6]);changed[4]=sealed(account)
                case(label,cur=current[:96]+b''.join(changed),e=actual_executable)
            for index,offset in ((2,22),(2,24),(2,32),(2,296),(2,352),(2,360),(3,48),(3,22),(4,22),(4,24),(4,32),(4,144),(4,328),(4,424),(4,544),(6,24),(6,80),(6,112),(6,120),(6,152),(6,184),(6,193)):
                value=bytes([parts[index][offset]^1]);changed_case(f'rehashed-{index}-{offset}',index,offset,value)
            # Forged executable bytes with fully rebound state/grant/account.
            badexe=exe[:-1]+bytes([exe[-1]^1]);changed_case('self-hashed-foreign-executable',6,152,H(badexe),badexe)
            disk=out/'current.img';disk.write_bytes(gpt(image.read_bytes()));before=sha(disk)
            native_bundle=out/'native.bundle';native_artifact=out/'native.artifact';native_exe=out/'native.executable'
            run([str(reader),str(disk),str(out/'witness'),str(native_bundle),str(native_artifact),str(native_exe),'1'],'native-readback')
            assert native_bundle.read_bytes()==current and native_artifact.read_bytes()==artifact and native_exe.read_bytes()==exe and sha(disk)==before
            run([str(verifier),str(out/'policy'),str(out/'selector'),str(out/'baseline'),str(native_bundle),str(native_artifact),str(native_exe),'2','2','1'],'native-current-proof')
            run(['e2fsck','-fn',str(image)],'linux-fsck')
            for path,data in (('/pkg/artifact.fpk',artifact),('/usr/bin/demo',exe)):
                dump=temp/('dump-'+path.rsplit('/',1)[1]);run(['debugfs','-R',f'dump {path} {dump}',str(image)],'linux-'+path.rsplit('/',1)[1]);assert dump.read_bytes()==data
            m['runtime']='PASS';m['disk_sha256']=before;m['baseline_unchanged']=True
    except Exception as e:m['runtime']='FAIL';m['error']=repr(e);raise
    finally:
        m['changed_inputs']=[p for p,h in guards.items() if sha(ROOT/p)!=h]
        m['changed_copies']=[p for p,h in m.get('compiled_inputs',{}).items() if sha(work/p)!=h]
        m['preservation']='FAIL' if m['changed_inputs'] or m['changed_copies'] else 'PASS'
        save();print(out,m['runtime'],m['preservation'],flush=True)
    assert m['preservation']=='PASS'
if __name__=='__main__':main()
