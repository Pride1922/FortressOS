"""One bounded private retirement integration/acceptance campaign. RAM disks only."""
import argparse,datetime,hashlib,json,os,pathlib,re,shutil,struct,subprocess,textwrap,tempfile,time
from concurrent.futures import ThreadPoolExecutor
from test_ext4_time_host import fixture
from test_pkg_enrollment import signed,policy
from test_pkg_service_integration import function
from pkg_account_service_overlay import apply
from pkg_mount_overlay import apply_mount
from pkg_retirement_idle_overlay import apply_idle as apply_retirement
from pkg_retirement_idle_vectors import completion
from pkg_mount_vectors import vectors,NOW
from test_pkg_retirement_auth import sign,PUB,anchor
ROOT=pathlib.Path(__file__).resolve().parents[1]
H=lambda b:hashlib.sha256(b).digest()
def put(b,o,v,f='Q'):struct.pack_into('<'+f,b,o,v)
def seal(b):b[-32:]=H(b[:-32])
def main():
 parser=argparse.ArgumentParser();parser.add_argument('--block',type=int,choices=(1024,2048,4096));parser.add_argument('--sector',type=int,choices=(512,4096));parser.add_argument('--smoke',action='store_true');parser.add_argument('--exhaustive',action='store_true');parser.add_argument('--jobs',type=int,default=4);args=parser.parse_args()
 assert not args.exhaustive or (args.block and args.sector),'name the exhaustive geometry explicitly'
 out=ROOT/'.codex-remote-attachments'/('pkg-retirement-idle-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'));work=out/'workspace';work.mkdir(parents=True)
 sha=lambda p:H(p.read_bytes()).hex();sources={}
 for folder in ('src','tests','user','scripts'):
  for p in (ROOT/folder).rglob('*'):
   if p.is_file() and p.suffix in ('.c','.h','.inc','.py','.mk') and '__pycache__' not in p.parts:
    name=str(p.relative_to(ROOT));sources[name]=sha(p);dst=work/name;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(p,dst)
 guards={str(p.relative_to(ROOT)):sha(p) for p in [ROOT/'AGENTS.md',ROOT/'README.md',ROOT/'assets/ssl/ca-certificates.sha256',*(p for p in (ROOT/'bin').iterdir() if p.name in ('fortress.elf','initramfs.tar','fortress.iso','fortress.img'))]}
 manifest=dict(status='INCOMPLETE',sources=sources,guards=guards,overlay=[],commands=[],cases=[],scope='private remaining-role retirement, escrow release, IDLE and fresh writer lifecycle; no production, installation, boot or physical acceptance')
 (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
 def replace(name,before,after):
  p=work/name;text=p.read_text();assert text.count(before)==1,(name,before,text.count(before));p.write_text(text.replace(before,after));manifest['overlay'].append(dict(path=name,before=before,after=after))
 # Reuse only the existing exact overlay construction; no old main/test campaign.
 source=(work/'scripts/test_pkg_mount_admission.py').read_text()
 start=source.index("    replace('src/fs/ext4_transaction.inc'");end=source.index('    env=dict(os.environ)',start)
 context=dict(work=work,replace=replace,function=function,apply=apply,apply_mount=apply_mount)
 exec(compile(textwrap.dedent(source[start:end]),'private-existing-mount-overlay','exec'),context)
 generated=context['generated'];apply_retirement(work,replace,generated)
 env=dict(os.environ);inputs=out/'fixture-inputs';extra=vectors(inputs);env.update(PKG_MOUNT_INPUT=str(inputs),PKG_MOUNT_PHASE='3',PKG_MOUNT_PUBLICATION='2')
 def run(cmd,label,timeout=180):
  started=time.monotonic();manifest['commands'].append(cmd);r=subprocess.run(cmd,cwd=work,text=True,capture_output=True,timeout=timeout,env=env)
  (out/(label+'.log')).write_text(r.stdout+r.stderr);assert r.returncode==0,(label,r.returncode,r.stdout[-1400:],r.stderr[-2400:]);manifest.setdefault('timings',[]).append(dict(label=label,seconds=time.monotonic()-started));return r.stdout+r.stderr
 try:
  flags=['gcc','-std=c11','-O1','-g','-fsanitize=address,undefined','-Wall','-Wextra','-Werror','-no-pie','-pthread','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fstack-usage']
  flags+=['-I'+p for p in ('tests','tests/ext4_host','tests/host','src/include','src/fs','src/drivers','src/kernel','src/arch/x86_64','src/mm','user/tools','src/lib/crypto/upstream/src','.')]
  objects=[]
  for tag,path in (('mono','src/lib/crypto/upstream/src/monocypher.c'),('ed','src/lib/crypto/upstream/src/optional/monocypher-ed25519.c')):
   obj=str(out/(tag+'.o'));run(flags+['-include','src/lib/crypto/namespace.h','-c',path,'-o',obj],'compile-'+tag);objects.append(obj)
  units=['tests/account-service-durable.c','tests/pkg_enrollment_candidate.c','tests/ext4_fault_disk.c','src/fs/permission_values.c','src/kernel/creds.c','user/tools/digest.c','src/lib/crypto/hash.c','src/lib/crypto/curve.c','src/lib/account_redo.c','user/tools/userdb.c','tests/pkg_mount_candidate.c','tests/pkg_grant_candidate.c','tests/pkg_retirement_candidate.c','tests/pkg_retirement_auth.c','user/pkg/catalog.c','user/pkg/verify.c','user/pkg/format.c','user/pkg/manifest.c','user/tools/gzip.c']
  dep=run(flags+['-MM','-MT','probe',*units],'dependencies')
  names={p for p in dep.replace('\\\n',' ').split() if not p.endswith(':')};names.update(('src/lib/crypto/upstream/src/monocypher.c','src/lib/crypto/upstream/src/optional/monocypher-ed25519.c','src/lib/crypto/namespace.h'))
  manifest['compiled_inputs']={p:sha(work/p) for p in names}
  run(flags+units+objects+['-o',str(out/'probe')],'compile')
  for bs in ((args.block,) if args.block else (1024,2048,4096)):
   image=fixture(out,bs,inodes=128,size_mib=32 if bs==4096 else 16);original=sha(image)
   for ss in ((args.sector,) if args.sector else (512,4096)):
    tag=f'{bs}-{ss}';pre=out/tag
    run([str(out/'probe'),'setup',str(image),str(ss),str(pre)],tag+'-setup')
    setup=pathlib.Path(str(pre)+'-setup.img');raw=setup.read_bytes();body=pathlib.Path(str(pre)+'-body.img').read_bytes();loc=pathlib.Path(str(pre)+'-locations.img').read_bytes()
    def read_record(i):
     r=body[416+128*i:544+128*i];n=struct.unpack_from('<Q',r,16)[0];parts=[]
     for j in range(struct.unpack_from('<H',r,32)[0]):
      logical,count,physical=struct.unpack_from('<IIQ',r,40+16*j);parts.append(raw[physical*bs:(physical+count)*bs])
     return bytearray(b''.join(parts)[:n])
    m,g,t,o,a=(read_record(i) for i in (0,1,2,4,5));put(a,20,4,'H')
    desc=signed(body);en=out/(tag+'-enrollment.bin');en.write_bytes(desc)
    art=(inputs/'artifact.fpk').read_bytes();exe=(inputs/'executable.bin').read_bytes();tr=(inputs/'trust.bin').read_bytes();state=(inputs/'next.bin').read_bytes();intent=(inputs/'pending.bin').read_bytes()
    c=bytearray(1024);c[:16]=b'FORT-PKG-RET-v1\0';put(c,20,1,'H');put(c,24,struct.unpack_from('<Q',body,24)[0]);c[32:112]=body[40:120];c[112:144]=H(desc);c[144:176]=H(m);c[176:208]=body[152:184];c[208:240]=a[80:112];c[240:256]=g[40:56]
    for off,b in ((256,tr),(288,state),(320,intent),(352,art),(384,exe)):c[off:off+32]=H(b)
    for off,v in ((416,len(art)),(424,len(exe)),(432,NOW-1),(440,NOW+60),(448,NOW),(456,2),(496,struct.unpack_from('<Q',g,24)[0]),(504,struct.unpack_from('<Q',a,24)[0]),(512,struct.unpack_from('<Q',a,144)[0])):put(c,off,v)
    c[464:496]=extra[336:368];c[520:584]=t[256:320];c[584:616]=H(g);c[616:648]=H(t);c[648:680]=anchor(a);c[680:712]=anchor(o,False);c=sign(c)
    cert=out/(tag+'-certificate.bin');cert.write_bytes(c)
    l=bytearray(1024);l[:16]=b'FORT-PKG-RLG-v1\0';put(l,16,1024,'I');put(l,20,1,'H');l[24:40]=g[24:40];l[40:104]=g[40:104];l[104:136]=H(c);l[136:168]=H(g);l[168:200]=H(t);l[200:208]=a[24:32];l[208:216]=a[144:152]
    l[216:220]=t[256:260];l[220:224]=t[268:272];l[224:232]=t[260:268];l[232:240]=t[288:296];l[240:288]=g[296:344];l[288:304]=t[296:312];seal(l);a[592:624]=H(l);seal(a)
    lp=out/(tag+'-ledger.bin');lp.write_bytes(l);ap=out/(tag+'-account.bin');ap.write_bytes(a)
    pol=out/(tag+'-policy.bin');pol.write_bytes(policy(desc,uuids=[body[40+16*i:56+16*i] for i in range(5)])+extra+b''.join(PUB[:3]).ljust(256,b'\0')+struct.pack('<II',3,2)+H(c)+loc)
    endcert,states=completion(out,tag,raw,body,m,g,t,l,a,o,c,desc,state,tr,art,exe)
    ep=out/(tag+'-completion.bin');ep.write_bytes(endcert)
    pol.write_bytes(pol.read_bytes()+H(endcert)+(out/(tag+'-completion-locator.img')).read_bytes())
    def probe(mode,img,label,rules=pol,descriptor=en,destination=None):
     cmd=[str(out/'probe'),mode,str(img),str(ss),str(descriptor),str(rules),str(out/label if destination is None else destination)]
     if mode.startswith('activate'):cmd += [str(lp),str(ap),str(cert),str(ep)]
     return run(cmd,label,timeout=300)
    activation=tag+'-activation';text=probe('activate',setup,activation);activation_events=int(re.search(r'PASS activation result=0 events=(\d+)',text)[1]);active=out/(activation+'-final.img')
    from pkg_retirement_idle_campaign import campaign
    campaign(out,tag,bs,ss,active,pol,en,probe,run,case_manifest=manifest,args=args,states=states,raw=raw,body=body)
  manifest['status']='PASS'
 except Exception as e:manifest['status']='FAIL';manifest['error']=repr(e);raise
 finally:
  manifest['changed_compiled_copies']=[p for p,h in manifest.get('compiled_inputs',{}).items() if sha(work/p)!=h]
  manifest['changed_repository_sources']=[p for p,h in sources.items() if sha(ROOT/p)!=h]
  manifest['changed_guards']=[p for p,h in guards.items() if sha(ROOT/p)!=h]
  if manifest['changed_compiled_copies'] or manifest['changed_repository_sources'] or manifest['changed_guards']:manifest['status']='FAIL'
  (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');print('Evidence:',out,manifest['status'],flush=True)
 assert manifest['status']=='PASS'
if __name__=='__main__':main()
