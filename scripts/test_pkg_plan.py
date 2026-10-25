"""Actual-code private typed plans; no filesystem mutations or boot acceptance."""
import datetime,json,pathlib,struct,subprocess,tempfile
from pkg_artifact_vectors import elf,pack,tar_header
from pkg_manifest_vectors import text
from pkg_enroll_records import H,PUB,keys,signed
ROOT=pathlib.Path(__file__).resolve().parents[1]
def main():
    out=ROOT/'.codex-remote-attachments'/('pkg-plan-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'));out.mkdir();m={'runtime':'INCOMPLETE','boot_acceptance':'NOT RUN','preservation':'INCOMPLETE','commands':[],'cases':[]}
    sources=[*list((ROOT/'src').rglob('*.c')),*list((ROOT/'user').rglob('*.c')),*list((ROOT/'tests').glob('pkg_plan*')),ROOT/'scripts/test_pkg_plan.py',ROOT/'Makefile',ROOT/'PROTECTED.md',ROOT/'AGENTS.md',ROOT/'docs/plans/PKG2_PRODUCTION_INTEGRATION.md'];guard={str(p.relative_to(ROOT)):H(p.read_bytes()).hex() for p in sources}
    def run(cmd,label):
        m['commands'].append(cmd);p=subprocess.run(cmd,cwd=ROOT,capture_output=True,text=True,timeout=180);(out/(label+'.log')).write_text(p.stdout+p.stderr);assert not p.returncode,(label,p.returncode,p.stdout[-2000:],p.stderr[-4000:])
    try:
        flags=['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-DTOOL_HOST_TEST','-I.','-Itests','-Itests/host','-Isrc/include','-Isrc/fs','-Isrc/kernel','-Isrc/mm','-Isrc/drivers','-Isrc/lib','-Isrc/lib/crypto/upstream/src']
        mono=[]
        for i,src in enumerate(('src/lib/crypto/upstream/src/monocypher.c','src/lib/crypto/upstream/src/optional/monocypher-ed25519.c')):
            p=out/f'mono{i}.o';run(flags+['-include','src/lib/crypto/namespace.h','-c',src,'-o',str(p)],f'crypto-{i}');mono.append(str(p))
        binary=out/'plan-host';run(flags+['tests/pkg_plan_candidate.c','tests/pkg_plan_host.c','user/pkg/catalog.c','user/pkg/verify.c','user/pkg/format.c','user/pkg/manifest.c','user/tools/gzip.c','user/tools/common.c','user/tools/digest.c','src/lib/crypto/hash.c','src/lib/crypto/curve.c',*mono,'-o',str(binary)],'compile')
        run(['gcc','-std=c11','-Os','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-mno-red-zone','-mno-sse','-mno-mmx','-msoft-float','-fstack-usage','-Wframe-larger-than=1024','-I.','-Itests','-Isrc/include','-c','tests/pkg_plan_candidate.c','-o',str(out/'plan-free.o')],'freestanding')
        with tempfile.TemporaryDirectory(prefix='fortress-plan-keys-') as td:
            paths=[]
            for i in (1,2,3):p=pathlib.Path(td)/str(i);p.write_bytes(bytes([i])*32);p.chmod(0o600);paths.append(p)
            signers=keys(paths)
            for label,docs,large in (('one',0,False),('sixteen',15,False),('nested',15,True),('max-executable',0,True)):
                package='toolbox' if label=='one' else 'demo';program='hello' if label=='one' else 'demo-app'
                directory=out/label;directory.mkdir();executable=elf()+(bytes(4*1024*1024-121) if large else b'');members=[('bin/'+program,executable,0o755,1)]+[(f'doc/{"nested/" if label=="nested" else ""}{i:02}/readme',b'documentation\n'*(1+i),0o644,2) for i in range(docs)]
                manifest=struct.pack('<4sHHQQQ',b'FMN1',1,len(members),73,1,3)+bytes(range(32))+text(package)+text(program)+text('73')+text('typed plan fixture');tar=b''
                for path,data,mode,role in members:manifest+=struct.pack('<BBHQ',role,0,mode,len(data))+H(data)+text(path);tar+=tar_header(path,data,mode)+data+bytes((-len(data))%512)
                artifact=pack(manifest,tar+bytes(1024));body=struct.pack('<16s16sQQQQHHI',b'FORT-PKG-CAT-v1\0',bytes([4])*16,3,7,1792843198,1792929600,1,2,0)+bytes([len(package)])+package.encode().ljust(32,b'\0')+struct.pack('<QQ',73,len(artifact))+H(artifact);catalog=signed(body,signers)
                # Native catalog policy: fixed byte fields, two unsigned ints,
                # three uint64s then digest. Independent from plan encoding.
                policy=bytes([4])*16+b''.join(PUB(k) for k in signers).ljust(256,b'\0')+struct.pack('<IIQQQ',3,2,3,7,1792843200)+H(catalog)
                for name,value in (('artifact',artifact),('catalog',catalog),('policy',policy)):(directory/name).write_bytes(value)
                run([str(binary),*[str(directory/n) for n in ('artifact','catalog','policy','plan')]],label)
                plan=(directory/'plan').read_bytes();assert len(plan)==8192 and H(plan[:-32])==plan[-32:] and struct.unpack_from('<H',plan,120)[0]==1+docs
                assert plan[128:160]==package.encode().ljust(32,b'\0') and plan[160:192]==program.encode().ljust(32,b'\0')
                data_blocks=sum((len(data)+4095)//4096 for _,data,_,_ in members);parents=set()
                for path,_,_,role in members:
                    if role==2:
                        split=path.split('/')
                        for i in range(1,len(split)):parents.add('/'.join(split[:i]))
                def maps(data):
                    leaves=(data+339)//340
                    return 0 if data<=4 else 2*(leaves+((leaves+339)//340 if leaves>4 else 0))
                parent_count=len(parents)+(2 if parents else 0);archive_blocks=(len(artifact)+4095)//4096;executable_blocks=(len(executable)+4095)//4096
                expected_blocks=8+2*(64+maps(64))+2+archive_blocks+maps(archive_blocks)+data_blocks+sum(maps((len(data)+4095)//4096) for _,data,_,_ in members)+executable_blocks+maps(executable_blocks)+2*(parent_count+len(members));expected_inodes=4+len(members)+parent_count+2
                assert struct.unpack_from('<QQ',plan,72)==(expected_blocks,expected_inodes) and struct.unpack_from('<I',plan,112)[0]==parent_count
                m['cases'].append({'label':label,'files':len(members),'blocks':expected_blocks,'inodes':expected_inodes,'result':'PASS'})
        m['runtime']='PASS'
    except BaseException as e:m['runtime']='FAIL';m['error']=repr(e);raise
    finally:
        m['changed_inputs']=[p for p,h in guard.items() if H((ROOT/p).read_bytes()).hex()!=h];m['preservation']='FAIL' if m['changed_inputs'] else 'PASS';m['inputs']=guard;(out/'manifest.json').write_text(json.dumps(m,indent=2)+'\n');print(out,m['runtime'],m['preservation'],flush=True)
if __name__=='__main__':main()
