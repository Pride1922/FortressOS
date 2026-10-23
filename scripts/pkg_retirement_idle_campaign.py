"""Combined completion/fresh-admission acceptance; disposable regular-file fixtures."""
import hashlib,pathlib,re,shutil,tempfile,struct,copy
from pkg_retirement_idle_vectors import completion,advance,seal
from test_pkg_retirement_auth import sign
from pkg_mount_vectors import NOW
from concurrent.futures import ThreadPoolExecutor
H=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
def campaign(out,tag,bs,ss,active,pol,en,probe,run,case_manifest,args,states,raw,body):
 case=dict(block=bs,sector=ss,status='INCOMPLETE',interruptions=[]);case_manifest['cases'].append(case)
 text=probe('finish',active,tag+'-finish');events=int(re.search(r'PASS IDLE events=(\d+)',text)[1]);final=out/(tag+'-finish-final.img')
 text=probe('recover',final,tag+'-again');assert 'commits=0' in text
 text=probe('fresh',final,tag+'-fresh');assert 'PASS fresh account writer' in text
 fresh=out/(tag+'-fresh-final.img');probe('recover',fresh,tag+'-fresh-recover')
 # Decode/patch records independently using the signed enrollment's maps.
 def read_record(image,loc):
  n=struct.unpack_from('<Q',loc,16)[0];data=b''
  for j in range(struct.unpack_from('<H',loc,32)[0]):
   logical,count,physical=struct.unpack_from('<IIQ',loc,40+16*j);data+=image[physical*bs:(physical+count)*bs]
  return data[:n]
 def store(image,loc,data):
  assert len(data)==struct.unpack_from('<Q',loc,16)[0];at=0
  for j in range(struct.unpack_from('<H',loc,32)[0]):
   logical,count,physical=struct.unpack_from('<IIQ',loc,40+16*j);take=min(count*bs,len(data)-at);image[physical*bs:physical*bs+take]=data[at:at+take];at+=take
  assert at==len(data)
 locs=[body[416+128*i:544+128*i] for i in (1,2)]+[(out/(tag+'-locations.img')).read_bytes()[:128]]+[body[416+128*i:544+128*i] for i in (5,4)]
 def audit(img,label,expected):
  run(['e2fsck','-fn',str(img)],label+'-fsck');image=img.read_bytes()
  assert [read_record(image,loc) for loc in locs]==[bytes(x) for x in expected]
  dumped=out/(label+'-demo.bin');run(['debugfs','-R',f'dump /usr/bin/demo {dumped}',str(img)],label+'-demo')
  assert dumped.read_bytes()==(out/'fixture-inputs/executable.bin').read_bytes()
  listing=run(['debugfs','-R','ls -l /pkg/transaction.v1',str(img)],label+'-transaction')
  assert all(line.split()[-1] in ('.','..') for line in listing.splitlines() if line.split() and line.split()[0].isdigit() and int(line.split()[0]))
 expected=states[11]
 for label,img in (('idle',final),('again',out/(tag+'-again-final.img'))):audit(img,tag+'-'+label,expected)
 fresh_expected=expected
 for lifetime in range(3):fresh_expected=advance(advance(fresh_expected,12),13)
 for label,img in (('fresh',fresh),('fresh-recover',out/(tag+'-fresh-recover-final.img'))):audit(img,tag+'-'+label,fresh_expected)
 probe('ordinary',final,tag+'-ordinary');ordinary=out/(tag+'-ordinary-final.img');probe('recover',ordinary,tag+'-ordinary-recover');audit(ordinary,tag+'-ordinary',expected);audit(out/(tag+'-ordinary-recover-final.img'),tag+'-ordinary-recover',expected)
 probe('death',final,tag+'-death');dead=out/(tag+'-death-final.img');probe('recover',dead,tag+'-death-recover')
 death_expected=advance(advance(states[11],12),13);audit(out/(tag+'-death-recover-final.img'),tag+'-death-recover',death_expected)
 # Terminal durable commit before checkpoint must be authenticated in preview.
 probe('terminal-pause',active,tag+'-terminal-pause');pending=out/(tag+'-terminal-pause-interrupted.img');text=probe('recover',pending,tag+'-terminal-replay');assert 'XR preview-newer=1' in text;audit(out/(tag+'-terminal-replay-final.img'),tag+'-terminal-replay',expected)
 refusals=[]
 def reject(label,image,rules=None):
  handle=tempfile.NamedTemporaryFile(prefix='fortress-idle-reject-',suffix='.img',delete=False);path=pathlib.Path(handle.name);assert path.parent==pathlib.Path('/tmp');handle.write(image);handle.close();before=H(path);label=tag+'-reject-'+label
  try:
   text=probe('reject',path,label,rules=pol if rules is None else rules);assert 'writes=0 flushes=0' in text and H(path)==before;refusals.append(label)
  except Exception:
   shutil.copyfile(path,out/(label+'-FAILED.img'));raise
  finally:path.unlink()
 for base_img in (active,final):
  for mask in range(1,31):
   image=bytearray(base_img.read_bytes())
   for i,loc in enumerate(locs):store(image,loc,states[11 if mask>>i&1 else 0][i])
   reject(('active' if base_img==active else 'idle')+f'-mixed-{mask}',image)
 endpoint=final.read_bytes();certloc=(out/(tag+'-completion-locator.img')).read_bytes()
 for off in (0,20,304,768,1040,1536,1727):
  image=bytearray(endpoint);c=bytearray(read_record(image,certloc));c[off]^=1;store(image,certloc,c);reject('completion-byte-'+str(off),image)
 # A coherent externally re-pinned forged signature still has no authority.
 image=bytearray(endpoint);c=bytearray(read_record(image,certloc));c[1536+32]^=1;store(image,certloc,c);rules=bytearray(pol.read_bytes());rules[1568:1600]=hashlib.sha256(c).digest();rp=out/(tag+'-forged.policy');rp.write_bytes(rules);reject('forged-repinned-signature',image,rp)
 for off in (384+32,384+336,760,1016,1020,1568,1604):
  rules=bytearray(pol.read_bytes());rules[off]^=1
  if off==384+32:struct.pack_into('<Q',rules,off,0)
  if off==1016:struct.pack_into('<I',rules,off,9)
  rp=out/(tag+'-bad-'+str(off)+'.policy');rp.write_bytes(rules);reject('policy-'+str(off),endpoint,rp)
 rules=bytearray(pol.read_bytes());rules[1568]^=1;rp=out/(tag+'-pending-wrong.policy');rp.write_bytes(rules);reject('committed-IDLE-wrong-policy',pending.read_bytes(),rp)
 # Actual inode identity/ownership/allocation refusals at the terminal endpoint.
 ipg=struct.unpack_from('<I',endpoint,1064)[0];desc_size=max(32,struct.unpack_from('<H',endpoint,1278)[0]);gd_start=(2 if bs==1024 else 1)*bs
 ino=struct.unpack_from('<I',states[0][1],324)[0];group,bit=divmod(ino-1,ipg);gd=gd_start+group*desc_size;table=struct.unpack_from('<I',endpoint,gd+8)[0];at=table*bs+bit*256
 for label,offset in (('exe-mode',0),('exe-uid',2),('exe-gid',24),('exe-links',26),('exe-size',4),('exe-generation',100)):
  image=bytearray(endpoint);image[at+offset]^=1;reject(label,image)
 image=bytearray(endpoint);bitmap=struct.unpack_from('<I',image,gd+4)[0];image[bitmap*bs+bit//8]^=1<<(bit%8);reject('exe-inode-bitmap',image)
 # Reject resurrection of the same retired inode generation after IDLE.
 old_ino=struct.unpack_from('<I',states[0][1],260)[0];group,bit=divmod(old_ino-1,ipg);gd=gd_start+group*desc_size;table=struct.unpack_from('<I',endpoint,gd+8)[0];old_at=table*bs+bit*256
 ordinary_bytes=ordinary.read_bytes();assert struct.unpack_from('<I',ordinary_bytes,old_at+100)[0]>struct.unpack_from('<I',endpoint,old_at+100)[0]
 image=bytearray(ordinary_bytes);image[old_at+100:old_at+104]=endpoint[old_at+100:old_at+104];reject('retired-generation-resurrection',image)
 # Signed pressure fixture: exact independent account reserve, no physical fill.
 b=copy.deepcopy(states[0]);g,t,l,a,o=b;reserve=(9*((8192+bs-1)//bs+int((8192+bs-1)//bs>4))+2-3,8)
 for d in range(2):
  free=struct.unpack_from('<I',raw,1036+4*d)[0];q=struct.unpack_from('<Q',g,328+8*d)[0];struct.pack_into('<Q',g,312+8*d,free-reserve[d]);struct.pack_into('<Q',g,296+8*d,free-reserve[d]+q)
 seal(g);oldcert=bytearray((out/(tag+'-certificate.bin')).read_bytes());oldcert[584:616]=hashlib.sha256(g).digest();oldcert=sign(oldcert);l[104:136]=hashlib.sha256(oldcert).digest()
 from pkg_retirement_idle_vectors import refresh
 refresh(b)
 newcert,pstates=completion(out,tag+'-pressure',raw,body,b'',*b,oldcert,en.read_bytes(),(out/'fixture-inputs/next.bin').read_bytes(),(out/'fixture-inputs/trust.bin').read_bytes(),(out/'fixture-inputs/artifact.fpk').read_bytes(),(out/'fixture-inputs/executable.bin').read_bytes())
 image=bytearray(active.read_bytes())
 for loc,data in zip(locs,b):store(image,loc,data)
 store(image,(out/(tag+'-locations.img')).read_bytes()[128:256],oldcert);store(image,certloc,newcert)
 rules=bytearray(pol.read_bytes());rules[1024:1056]=hashlib.sha256(oldcert).digest();rules[1568:1600]=hashlib.sha256(newcert).digest();rp=out/(tag+'-pressure.policy');rp.write_bytes(rules);pi=out/(tag+'-pressure.img');pi.write_bytes(image)
 text=probe('pressure',pi,tag+'-pressure',rules=rp);assert 'PASS competing block/inode allocations refused' in text
 pressure=out/(tag+'-pressure-final.img');probe('recover',pressure,tag+'-pressure-again',rules=rp);audit(pressure,tag+'-pressure',pstates[11]);audit(out/(tag+'-pressure-again-final.img'),tag+'-pressure-again',pstates[11])
 case.update(status='PASS',events=events,linux_audits=10,refusals=refusals,reserve=reserve)
 print(tag,'IDLE, replay and fresh writer PASS',flush=True)
 if args.smoke:return
 scratch=pathlib.Path(tempfile.mkdtemp(prefix='fortress-idle-'));assert scratch.parent==pathlib.Path('/tmp');guards={p:H(p) for p in (active,pol,en)};local_active=scratch/'source.img';shutil.copyfile(active,local_active);assert H(local_active)==H(active)
 def cut(task):
  profile,position=task;label=tag+f'-io-{profile}-{position}';paths=[]
  try:
   prefix=scratch/label;probe(f'finish={profile}:{position}',local_active,label,destination=prefix);broken=pathlib.Path(str(prefix)+'-interrupted.img');paths.append(broken)
   digests=[]
   for restart in range(2):
    dest=scratch/(label+f'-restart{restart}');text=probe('recover',broken,label+f'-restart{restart}',destination=dest);new=pathlib.Path(str(dest)+'-final.img');paths.append(new)
    assert [read_record(new.read_bytes(),loc) for loc in locs]==[bytes(x) for x in states[11]]
    if restart:assert 'commits=0' in text
    digests.append(H(new));broken.unlink();broken=new
   broken.unlink();return dict(profile=profile,cut=position,restarts=2,sha256=digests)
  except Exception:
   keep=out/'retained-failures'/label;keep.mkdir(parents=True,exist_ok=True)
   for p in paths:
    if p.exists():shutil.copyfile(p,keep/p.name)
   raise
 try:
  tasks=[(p,c) for p in range(8) for c in (range(events) if args.exhaustive else sorted({0,events//2,events-1}))]
  with ThreadPoolExecutor(max_workers=args.jobs) as pool:case['interruptions']=list(pool.map(cut,tasks))
 finally:
  assert H(local_active)==H(active)
  assert scratch.parent==pathlib.Path('/tmp') and scratch.name.startswith('fortress-idle-');shutil.rmtree(scratch)
 assert all(H(p)==h for p,h in guards.items())
