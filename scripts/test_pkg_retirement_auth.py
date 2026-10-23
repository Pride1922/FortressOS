"""Private authenticated retirement proof. Public fixture keys; no I/O adapter."""
import copy,datetime,hashlib,json,pathlib,re,shutil,struct,subprocess
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding,PublicFormat
from pkg_mount_vectors import vectors,PROFILE,NOW
from test_pkg_enrollment import body,signed,policy
from test_pkg_retirement import fixture,rehash
ROOT=pathlib.Path(__file__).resolve().parents[1]
H=lambda b:hashlib.sha256(b).digest()
def put(b,o,v,f='Q'):struct.pack_into('<'+f,b,o,v)
def seal(b):b[-32:]=H(b[:-32])
KEYS=[Ed25519PrivateKey.from_private_bytes(bytes([i])*32) for i in range(71,80)]
PUB=[k.public_key().public_bytes(Encoding.Raw,PublicFormat.Raw) for k in KEYS]
def sign(c,signers=(0,1)):
 c=bytearray(c[:1024]);put(c,16,1024+96*len(signers),'I');put(c,22,len(signers),'H')
 return c+b''.join(i+s for i,s in sorted((H(PUB[k]),KEYS[k].sign(bytes(c))) for k in signers))
def anchor(a,account=True):
 a=bytearray(a);a[24:32]=bytes(8);a[992:1024]=bytes(32)
 if account:
  for lo,hi in ((112,144),(440,480),(592,624)):a[lo:hi]=bytes(hi-lo)
 return H(a)
def refresh(x):
 b=x['b'];rehash(b)
def create(out,**kwargs):
 extra=vectors(out/'evidence',**kwargs);art=(out/'evidence/artifact.fpk').read_bytes();exe=(out/'evidence/executable.bin').read_bytes()
 st=(out/'evidence/next.bin').read_bytes();tr=(out/'evidence/trust.bin').read_bytes();intent=(out/'evidence/pending.bin').read_bytes();old=(out/'evidence/old.bin').read_bytes()
 b=fixture(blocks=1,seq=10,account_seq=50);m,g,t,l,a,o=b
 m[112:144]=PROFILE;g[104:136]=PROFILE;t[104:136]=PROFILE
 g[40:56]=intent[24:40];t[32:48]=intent[24:40];l[40:56]=intent[24:40]
 g[136:168]=H(intent);g[168:200]=H(old);g[200:232]=H(st);g[232:264]=H(tr)
 for off,val in ((360,1),(368,2),(376,3),(384,7)):put(g,off,val)
 g[392:424]=H(H(st)+H(art)+PROFILE)
 t[256:8160]=bytes(7904);put(t,22,4,'H')
 for i,(role,size) in enumerate(((1,len(art)),(17,len(exe)),(18,1024),(19,1024))):
  at=256+64*i;struct.pack_into('<HHIII',t,at,role,1,500+i,7,1);t[at+16:at+32]=g[40:56];struct.pack_into('<QQQ',t,at+32,size,1,1)
 for i in range(2):
  put(g,296+8*i,(16,6)[i]);put(g,312+8*i,(12,2)[i]);put(g,328+8*i,4)
 struct.pack_into('<HHIIIQ',l,216,1,1,1,500,7,len(art))
 for off,vals in ((240,(16,6)),(256,(12,2)),(272,(4,4)),(288,(1,1))):
  struct.pack_into('<QQ',l,off,*vals)
 a[480:512]=H(tr);a[512:544]=H(st);a[544:576]=H(intent);a[576:592]=g[40:56]
 rehash(b)
 en=body();put(en,24,1);en[120:152]=H(m);en[152:184]=PROFILE;en[248:280]=H(tr);en[280:312]=H(extra[336:376]);en[312:344]=a[80:112]
 for i in range(3):put(en,416+128*(i+1)+4,44+i,'I');put(en,416+128*(i+1)+8,1,'I')
 en=signed(en)
 c=bytearray(1024);c[:16]=b'FORT-PKG-RET-v1\0';put(c,20,1,'H');put(c,24,1);c[32:112]=m[32:112];c[112:144]=H(en);c[144:176]=H(m);c[176:208]=PROFILE;c[208:240]=a[80:112];c[240:256]=g[40:56]
 for off,v in ((256,tr),(288,st),(320,intent),(352,art),(384,exe)):c[off:off+32]=H(v)
 for off,val in ((416,len(art)),(424,len(exe)),(432,NOW-1),(440,NOW+60),(448,NOW),(456,2),(496,10),(504,50),(512,9)):put(c,off,val)
 c[464:496]=extra[336:368];c[520:584]=t[256:320];c[584:616]=H(g);c[616:648]=H(t);c[648:680]=anchor(a);c[680:712]=anchor(o,False)
 c=sign(c);l[104:136]=H(c);rehash(b)
 return dict(b=b,en=en,certificate=c,extra=bytearray(extra),ep=policy(en,epoch=1),rp=b''.join(PUB[:3]).ljust(256,b'\0')+struct.pack('<II',3,2)+H(c),state=st,trust=tr,intent=intent,art=art,exe=exe)
def main():
 out=ROOT/'.codex-remote-attachments'/('pkg-retirement-auth-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'));work=out/'workspace';work.mkdir(parents=True)
 sources={}
 for folder in ('src','tests','user','scripts'):
  for p in (ROOT/folder).rglob('*'):
   if p.is_file() and p.suffix in ('.c','.h','.inc','.py','.mk') and '__pycache__' not in p.parts:
    name=str(p.relative_to(ROOT));sources[name]=H(p.read_bytes()).hex();dst=work/name;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(p,dst)
 guards={str(p.relative_to(ROOT)):H(p.read_bytes()).hex() for p in [ROOT/'AGENTS.md',ROOT/'README.md',*(p for p in (ROOT/'bin').iterdir() if p.name in ('fortress.elf','initramfs.tar','fortress.iso','fortress.img'))]}
 manifest=dict(status='INCOMPLETE',sources=sources,guards=guards,commands=[],cases=[],scope='pure authenticated retirement certificate and evidence gate; no filesystem, mount, boot, signer provisioning or production integration')
 lines=[]
 def save(x,label,expected):
  name=f'{len(lines):04d}-{label}.bin';b=x['b'];en,c,art,exe=(x[k] for k in ('en','certificate','art','exe'))
  wire=x['ep']+x['extra']+x['rp']+struct.pack('<IIII',len(en),len(c),len(art),len(exe))+b''.join(b)+x['state']+x['trust']+x['intent']+en+c+art+exe
  assert len(x['ep'])==384 and len(x['extra'])==376 and len(x['rp'])==296
  (out/name).write_bytes(wire);lines.append(f'{expected} {name}\n');manifest['cases'].append(dict(label=label,expected=expected))
 def resign(x,signers=(0,1)):
  x['certificate']=sign(x['certificate'],signers);x['rp']=x['rp'][:264]+H(x['certificate']);x['b'][3][104:136]=H(x['certificate']);refresh(x)
 x=create(out);save(x,'initial',1)
 save(create(out/'forged-catalog',bad_signature=True),'coherent-forged-catalog',0)
 save(create(out/'unsupported-manifest',with_doc=True),'authorized-unsupported-manifest',0)
 chain=[copy.deepcopy(x)]
 for step in range(3):
  y=copy.deepcopy(chain[-1]);m,g,t,l,a,o=y['b'];put(g,24,11+step);put(t,48,11+step);put(l,24,11+step);put(l,200,51+step);put(a,24,51+step);put(a,440,11+step);put(o,24,51+step)
  if step==0:put(t,268,2,'I');put(l,220,2,'I')
  if step==1:
   put(t,296,0);put(g,312,13);put(g,328,3);put(l,256,13);put(l,272,3);put(l,288,0)
  if step==2:
   put(t,258,3,'H');put(t,268,0,'I');put(t,288,0);put(t,304,0);put(g,320,3);put(g,336,3);put(l,218,3,'H');put(l,220,0,'I');put(l,232,0);put(l,264,3);put(l,280,3);put(l,296,0)
  refresh(y);chain.append(y);save(y,'recovery-'+str(step),1)
 # Historical exact recovery survives catalog/certificate expiry.
 for i,z in enumerate(chain):
  y=copy.deepcopy(z);put(y['extra'],32,NOW+3600);save(y,'expired-exact-'+str(i),1)
 for label,mutate in [
  ('wrong-retirement-key',lambda y:y.__setitem__('rp',bytes(32)+y['rp'][32:])),
  ('duplicate-policy-key',lambda y:y.__setitem__('rp',y['rp'][:32]*2+y['rp'][64:])),
  ('zero-threshold',lambda y:y.__setitem__('rp',y['rp'][:260]+bytes(4)+y['rp'][264:])),
  ('wrong-capability-pin',lambda y:y.__setitem__('rp',y['rp'][:264]+bytes(32))),
  ('wrong-enrollment-key',lambda y:y.__setitem__('ep',bytes(32)+y['ep'][32:])),
  ('wrong-base',lambda y:put(y['extra'],368,3)),
  ('future-intent',lambda y:put(y['extra'],32,NOW-1)),
  ('zero-clock',lambda y:put(y['extra'],32,0)),
  ('unbounded-clock',lambda y:put(y['extra'],32,4102444801)),
  ('catalog-floor',lambda y:y['extra'].__setitem__(40,0)),
  ('wrong-catalog-key',lambda y:y['extra'].__setitem__(80,0)),
  ('bad-executable',lambda y:y.__setitem__('exe',y['exe'][:-1]+bytes([y['exe'][-1]^1]))),
  ('bad-archive',lambda y:y.__setitem__('art',y['art'][:-1]+bytes([y['art'][-1]^1])))]:
  y=copy.deepcopy(x);mutate(y);save(y,label,0)
 # Valid signatures cannot authorize inconsistent or unsupported contexts.
 for off,value,fmt in ((20,2,'H'),(24,2,'Q'),(416,len(x['art'])+1,'Q'),(432,NOW+1,'Q'),(440,NOW-1,'Q'),(440,NOW,'Q'),(448,NOW-1,'Q'),(456,3,'Q'),(496,9,'Q'),(504,49,'Q'),(512,10,'Q'),(520,2,'H'),(524,501,'I'),(528,8,'I'),(532,2,'I'),(552,len(x['art'])+1,'Q'),(560,2,'Q'),(568,2,'Q')):
  y=copy.deepcopy(x);put(y['certificate'],off,value,fmt);resign(y);save(y,'signed-field-'+str(off),0)
 y=copy.deepcopy(x);y['rp']=b''.join(PUB[:8])+struct.pack('<II',8,8)+y['rp'][264:];resign(y,tuple(range(8)));save(y,'eight-of-eight',1)
 for count,threshold in ((0,1),(9,2),(3,4)):
  y=copy.deepcopy(x);y['rp']=y['rp'][:256]+struct.pack('<II',count,threshold)+y['rp'][264:];save(y,f'policy-bounds-{count}-{threshold}',0)
 for off in (0,32,112,144,176,208,240,256,288,320,352,384,464,584,616,648,680,712,1023):
  y=copy.deepcopy(x);y['certificate'][off]^=1;resign(y);save(y,'signed-binding-'+str(off),0)
 # Integrity pin updates must never replace actual signature verification.
 for part in ('retirement','enrollment','catalog'):
  y=copy.deepcopy(x)
  if part=='retirement':
   y['certificate'][-1]^=1;y['rp']=y['rp'][:264]+H(y['certificate']);y['b'][3][104:136]=H(y['certificate']);refresh(y)
  elif part=='enrollment':
   en=bytearray(y['en']);en[-1]^=1;y['en']=bytes(en);y['ep']=policy(en,epoch=1);y['certificate'][112:144]=H(en);resign(y)
  else:
   tr=bytearray(y['trust']);cn=struct.unpack_from('<I',tr,96)[0];tr[256+cn-1]^=1;tr[64:96]=H(tr[256:256+cn]);seal(tr);y['trust']=bytes(tr)
   # A coherent full evidence rewrite is covered separately below.
  save(y,'forged-'+part,0)
 y=copy.deepcopy(x);resign(y,(0,1,2));y['certificate'][-1]^=1;y['rp']=y['rp'][:264]+H(y['certificate']);y['b'][3][104:136]=H(y['certificate']);refresh(y);save(y,'invalid-extra-signature-after-quorum',0)
 for off in range(712,1024):
  y=copy.deepcopy(x);y['certificate'][off]=1;resign(y);save(y,f'signed-reserved-{off}',0)
 for signers,expected in (((0,),0),((0,1,2),1),((0,1,3),0),((0,0),0)):
  y=copy.deepcopy(x);resign(y,signers);save(y,'signers-'+''.join(map(str,signers)),expected)
 # Rehashed mutable-record substitutions: recomputing checksums is not authority.
 for step in range(4):
  for record,off in ((1,312),(2,260+64),(3,208),(4,328),(4,144),(4,32),(5,40)):
   y=copy.deepcopy(chain[step]);y['b'][record][off]^=1;refresh(y);save(y,f'rewrite-{step}-{record}-{off}',0)
 for step,z in enumerate(chain):
  for label,mutate in (
   ('canonical-account',lambda b:b[4].__setitem__(328,99)),
   ('operation-generation',lambda b:(put(b[4],32,5),put(b[5],40,5))),
   ('credential-generation',lambda b:(put(b[4],144,10),put(b[3],208,10))),
   ('other-ownership',lambda b:put(b[2],328,8,'I')),
   ('logical-size-increase',lambda b:(put(b[2],288,len(x['art'])+1),put(b[3],232,len(x['art'])+1))),
   ('logical-size-decrease',lambda b:(put(b[2],288,len(x['art'])-1),put(b[3],232,len(x['art'])-1)))):
   if step==3 and label.startswith('logical-size'):continue
   y=copy.deepcopy(z);mutate(y['b']);refresh(y);save(y,f'valid-records-{step}-{label}',0)
 y=copy.deepcopy(chain[2]);m,g,t,l,a,o=y['b']
 for rec,off in ((g,24),(t,48),(l,24)):put(rec,off,11)
 for rec,off in ((l,200),(a,24),(o,24)):put(rec,off,51)
 put(a,440,11);refresh(y);save(y,'valid-records-folded-unlink',0)
 y=copy.deepcopy(chain[2]);m,g,t,l,a,o=y['b']
 for rec,off in ((l,200),(a,24),(o,24)):put(rec,off,51)
 refresh(y);save(y,'valid-records-account-generation-mismatch',0)
 for i in range(4):
  for j in range(4):
   if i==j:continue
   for record in range(1,6):
    y=copy.deepcopy(chain[i]);y['b'][record]=copy.deepcopy(chain[j]['b'][record]);save(y,f'mixed-{i}-{j}-{record}',0)
 # Missing/trailing certificate and enrollment inputs.
 for part in ('certificate','en'):
  for n in (0,1,1023,len(x[part])-1,len(x[part])+1):
   y=copy.deepcopy(x);y[part]=(y[part]+b'\0')[:n];save(y,f'length-{part}-{n}',0)
 (out/'cases.txt').write_text(''.join(lines))
 def run(cmd,label,timeout=180):
  manifest['commands'].append(cmd);r=subprocess.run(cmd,cwd=work if label.startswith('compile') or label=='dependencies' else out,capture_output=True,text=True,timeout=timeout)
  (out/(label+'.log')).write_text(r.stdout+r.stderr);assert r.returncode==0,(label,r.returncode,r.stderr[-3000:],r.stdout[-1000:]);return r.stdout
 flags=['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-ffunction-sections','-fdata-sections','-I.','-Itests','-Isrc/include','-Isrc/kernel','-Isrc/fs','-Isrc/drivers','-Isrc/mm','-Iuser/tools','-Isrc/lib/crypto/upstream/src']
 units=['tests/pkg_retirement_auth.c','tests/pkg_retirement_auth_host.c','tests/pkg_retirement_candidate.c','tests/pkg_mount_candidate.c','tests/pkg_enrollment_candidate.c','tests/pkg_grant_candidate.c','src/lib/crypto/hash.c','src/lib/crypto/curve.c','user/tools/digest.c','user/pkg/catalog.c','user/pkg/verify.c','user/pkg/format.c','user/pkg/manifest.c','user/tools/gzip.c']
 try:
  objects=[]
  for tag,path in (('mono','src/lib/crypto/upstream/src/monocypher.c'),('ed','src/lib/crypto/upstream/src/optional/monocypher-ed25519.c')):
   obj=str(out/(tag+'.o'));run(flags+['-include','src/lib/crypto/namespace.h','-c',path,'-o',obj],'compile-'+tag);objects.append(obj)
  dep=run(flags+['-MM','-MT','probe',*units],'dependencies')
  names={p for p in dep.replace('\\\n',' ').split() if not p.endswith(':')}
  names.update(('src/lib/crypto/upstream/src/monocypher.c','src/lib/crypto/upstream/src/optional/monocypher-ed25519.c','src/lib/crypto/namespace.h'))
  manifest['compiled_inputs']={p:H((work/p).read_bytes()).hex() for p in names}
  run(flags+units+objects+['-no-pie','-Wl,--gc-sections','-o',str(out/'probe')],'compile-host')
  report=run([str(out/'probe'),'cases.txt'],'host');print(report,end='')
  run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-mno-red-zone','-mno-sse','-mno-mmx','-msoft-float','-fstack-usage','-Wframe-larger-than=1024','-I.','-Itests','-Isrc/include','-Isrc/kernel','-Isrc/fs','-Isrc/drivers','-Isrc/mm','-Iuser/tools','-c','tests/pkg_retirement_auth.c','-o',str(out/'auth.o')],'compile-freestanding')
  manifest['report']=report;manifest['status']='PASS'
 except Exception as e:manifest['status']='FAIL';manifest['error']=repr(e);raise
 finally:
  manifest['changed_sources']=[p for p,h in sources.items() if H((ROOT/p).read_bytes()).hex()!=h]
  manifest['changed_copies']=[p for p,h in sources.items() if H((work/p).read_bytes()).hex()!=h]
  manifest['changed_guards']=[p for p,h in guards.items() if H((ROOT/p).read_bytes()).hex()!=h]
  if manifest['changed_sources'] or manifest['changed_copies'] or manifest['changed_guards']:manifest['status']='FAIL'
  (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');print('Evidence:',out,manifest['status'],flush=True)
 assert manifest['status']=='PASS'
if __name__=='__main__':main()
