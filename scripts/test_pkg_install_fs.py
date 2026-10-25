"""Actual private EXT4/JBD2 install/remove executor, disposable host images."""
import datetime,json,pathlib,shutil,subprocess,tempfile,os,struct,hmac,hashlib
from pkg_enroll_records import H,keys
from pkg_install_fixture import fixture
from pkg_install_overlay import overlay
from pkg_pending_records import observation
from pkg_enroll_fixture import FILES,SIZES
ROOT=pathlib.Path(__file__).resolve().parents[1]

def oracle(directory,record_key):
    image=directory/'after.img';initial=directory/'root.img';config=(directory/'config').read_bytes()
    raw=image.read_bytes();physical=struct.unpack_from('<Q',config,24)[0];record=raw[physical*4096:physical*4096+8192]
    assert hmac.compare_digest(record[8160:],hmac.new(record_key,record[:8160],hashlib.sha256).digest())
    assert struct.unpack_from('<QQQQ',record,32)==(4,73,3,7)
    expected=json.loads((directory/'expected.json').read_text());mask=(1<<len(expected))-1
    assert struct.unpack_from('<7I',record,64)==(0,1,0,mask,mask,0,0)
    assert not any(record[96:112]+record[144:256]+record[896:904])
    for i,item in enumerate(expected):
        obj=observation(image,item['path'],i)
        _,ino,gen,mode,uid,gid,links,_=struct.unpack_from('<8I',obj)
        assert mode==item['mode'] and not uid and not gid and links==1
        assert obj[48:80].hex()==item['sha256'] and struct.unpack_from('<Q',obj,32)[0]==item['bytes']
        assert struct.unpack_from('<IIQ',record,512+16*i)==(ino,gen,item['bytes'])
    for path in [*FILES,*('/pkg/'+n for n in SIZES)]:
        assert observation(initial,path,100)==observation(image,path,100),path
    (directory/'oracle.json').write_text(json.dumps({'result':'PASS','files':expected,'generation':4,'build_floor':73,'root_floor':3,'catalog_floor':7,'baseline_objects_preserved':len(FILES)+len(SIZES)},indent=2)+'\n')
def main():
    out=ROOT/'.codex-remote-attachments'/('pkg-install-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'));out.mkdir();work=out/'workspace';work.mkdir()
    m={'runtime':'INCOMPLETE','preservation':'INCOMPLETE','boot_acceptance':'NOT RUN','workspace':str(work),'commands':[],'cases':[]};guard={str(p.relative_to(ROOT)):H(p.read_bytes()).hex() for folder in ('src','user','tests','scripts') for p in (ROOT/folder).rglob('*') if p.is_file() and '__pycache__' not in p.parts};guard.update({p:H((ROOT/p).read_bytes()).hex() for p in ('Makefile','PROTECTED.md','AGENTS.md','docs/plans/PKG2_PRODUCTION_INTEGRATION.md')})
    def save():(out/'manifest.json').write_text(json.dumps(m,indent=2)+'\n')
    def run(cmd,label):
        m['commands'].append(cmd);save();p=subprocess.run(cmd,cwd=work,capture_output=True,text=True,timeout=300);(out/(label+'.log')).write_text(p.stdout+p.stderr);assert not p.returncode,(label,p.returncode,p.stdout[-2000:],p.stderr[-5000:])
    def replace(name,before,after):
        p=work/name;s=p.read_text();assert s.count(before)==1,(name,before,s.count(before));p.write_text(s.replace(before,after))
    try:
        for folder in ('src','user','tests'):shutil.copytree(ROOT/folder,work/folder,ignore=shutil.ignore_patterns('__pycache__'))
        overlay(work,replace)
        flags=['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-pthread','-DTOOL_HOST_TEST','-DFORTRESS_DAC_ENFORCED','-I.','-Itests','-Itests/ext4_host','-Itests/host','-Isrc/include','-Isrc/fs','-Isrc/drivers','-Isrc/mm','-Isrc/lib','-Isrc/kernel','-Iuser/tools','-Isrc/lib/crypto/upstream/src']
        objects=[]
        for i,src in enumerate(('src/lib/crypto/upstream/src/monocypher.c','src/lib/crypto/upstream/src/optional/monocypher-ed25519.c')):
            obj=out/f'mono{i}.o';run(flags+['-include','src/lib/crypto/namespace.h','-c',src,'-o',str(obj)],f'crypto-{i}');objects.append(str(obj))
        binary=out/'install-native';pure=['tests/pkg_plan_candidate.c','user/pkg/mutation_cli.c','user/pkg/transaction_cli.c','user/pkg/cli.c','user/pkg/catalog.c','user/pkg/verify.c','user/pkg/format.c','user/pkg/manifest.c','user/tools/gzip.c','user/tools/common.c','user/tools/digest.c','src/lib/crypto/hash.c','src/lib/crypto/curve.c']
        run(flags+pure+['tests/pkg_install_fs_host.c','tests/ext4_fault_disk.c','src/fs/permission_values.c','src/kernel/creds.c','src/lib/account_redo.c',*objects,'-o',str(binary)],'compile')
        bridge=(work/'src/fs/ext4.c').read_text()+'''\nint px_compile_prepare(const px_policy *p,const uint8_t *a,size_t an,const uint8_t *c,size_t cn,const uint8_t *n,size_t nn){return px_prepare(p,a,an,c,cn,n,nn,true);}
int px_compile_control(unsigned op){return px_control(op);}
'''
        (work/'src/fs/pkg_install_compile.c').write_text(bridge)
        kernel_flags=['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-DFORTRESS_DAC_ENFORCED','-ffreestanding','-fno-stack-protector','-fno-stack-check','-fno-lto','-fPIE','-m64','-march=x86-64','-mno-80387','-mno-mmx','-mno-sse','-mno-sse2','-mno-red-zone','-fstack-usage','-I.','-Isrc/include','-Isrc/drivers','-Isrc/arch/x86_64','-Isrc/mm','-Isrc/kernel','-Isrc/lib','-Isrc/fs','-Isrc/net']
        run(kernel_flags+['-c','src/fs/pkg_install_compile.c','-o',str(out/'kernel-surface.o')],'kernel-compile')
        frames={line.split('\t')[0]:int(line.split('\t')[1]) for line in (out/'kernel-surface.su').read_text().splitlines() if ':px_' in line}
        assert frames and max(frames.values())<=1024,frames
        m['private_stack_frames']=frames
        m['compiled_inputs']={str(p.relative_to(work)):H(p.read_bytes()).hex() for p in work.rglob('*') if p.is_file()};save()
        with tempfile.TemporaryDirectory(prefix='fortress-install-keys-') as td:
            paths=[]
            for i in (1,2,3):p=pathlib.Path(td)/str(i);p.write_bytes(bytes([i])*32);p.chmod(0o600);paths.append(p)
            signers=keys(paths)
            key_file=pathlib.Path(td)/'record-key';record_key=os.urandom(32);key_file.write_bytes(record_key);key_file.chmod(0o600);m['record_key_fingerprint']=H(record_key).hex()
            for label,docs,large,low in (('one',0,False,False),('sixteen',15,False,False),('large',15,True,False),('low-space',15,True,True)):
                directory=out/label;image=fixture(directory,signers,record_key,docs,large,low);run(['e2fsck','-fn',str(image)],label+'-initial-fsck')
                argv=[str(binary),str(image),*[str(directory/n) for n in ('artifact','catalog','policy','config')],str(directory/'after.img'),'-3' if low else '40',str(key_file)]
                run(argv,label+'-runtime')
                if not low:
                    run(['e2fsck','-fn',str(directory/'after.img')],label+'-final-fsck');oracle(directory,record_key)
                    if docs:
                        foreign=directory/'foreign.img';shutil.copyfile(directory/'after.img',foreign);payload=directory/'foreign';payload.write_bytes(b'foreign bytes\n')
                        run(['debugfs','-w','-R',f'write {payload} /usr/share/pkg/toolbox/73/doc/foreign',str(foreign)],label+'-foreign-seed')
                        run(['e2fsck','-fn',str(foreign)],label+'-foreign-fsck')
                        forged=[*argv];forged[1]=str(foreign);forged[7]='-4';run(forged,label+'-foreign-refusal')
                m['cases'].append({'profile':label,'result':'PASS'});save()
        m['runtime']='PASS'
    except BaseException as e:m['runtime']='FAIL';m['error']=repr(e);raise
    finally:
        m['changed_inputs']=[p for p,h in guard.items() if not (ROOT/p).is_file() or H((ROOT/p).read_bytes()).hex()!=h];m['preservation']='FAIL' if m['changed_inputs'] else 'PASS';save();print(out,m['runtime'],m['preservation'],flush=True)
if __name__=='__main__':main()
