"""Private selector, initial-IDLE bundle and offline publication acceptance.

Independent Python wire/signature oracle versus actual C validators. No kernel,
release build, production key or block-device write is performed.
"""
import datetime,hashlib,json,os,pathlib,re,shutil,struct,subprocess,tempfile
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding,PublicFormat
from pkg_test_enroll import prepare,publish,NAMES
from pkg_test_enroll_read import inspect
ROOT=pathlib.Path(__file__).resolve().parents[1]
H=lambda b:hashlib.sha256(b).digest()
PUB=lambda k:k.public_key().public_bytes(Encoding.Raw,PublicFormat.Raw)

def main():
 out=ROOT/'.codex-remote-attachments'/('pkg-selector-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'));out.mkdir(parents=True);work=out/'workspace';work.mkdir()
 m={'result':'INCOMPLETE','commands':[],'vectors':[],'cuts':[],'scope':'private TEST selector and prepared record artifact publication; no root filesystem enrollment, boot, production trust or physical durability acceptance'}
 sha=lambda p:H(p.read_bytes()).hex()
 guards={str(p.relative_to(ROOT)):sha(p) for p in [ROOT/'Makefile',ROOT/'AGENTS.md',ROOT/'PROTECTED.md',ROOT/'docs/plans/PKG2_PRODUCTION_INTEGRATION.md',ROOT/'assets/ssl/ca-certificates.sha256',*(p for p in (ROOT/'bin').iterdir() if p.is_file())]};m['guards']=guards
 def save():(out/'manifest.json').write_text(json.dumps(m,indent=2)+'\n')
 def run(cmd,label,cwd=work,expected=0):
  m['commands'].append(cmd);r=subprocess.run(cmd,cwd=cwd,capture_output=True,text=True,timeout=180);(out/(label+'.log')).write_text(r.stdout+r.stderr);assert r.returncode==expected,(label,r.returncode,r.stderr[-2400:],r.stdout[-1000:]);return r.stdout
 flags=['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-DTOOL_HOST_TEST','-I.','-Itests','-Isrc/include','-Isrc/fs','-Isrc/kernel','-Iuser/tools','-Isrc/lib/crypto/upstream/src']
 units=['tests/pkg_selector_candidate.c','tests/pkg_selector_host.c','tests/pkg_enrollment_candidate.c','tests/pkg_grant_candidate.c','tests/pkg_mount_candidate.c','user/pkg/catalog.c','user/pkg/verify.c','user/pkg/format.c','user/pkg/manifest.c','user/tools/gzip.c','user/tools/common.c','user/tools/digest.c','src/lib/crypto/hash.c','src/lib/crypto/curve.c']
 mono=['src/lib/crypto/upstream/src/monocypher.c','src/lib/crypto/upstream/src/optional/monocypher-ed25519.c']
 try:
  # Dependency discovery reads source only; all compiled bytes are copied and
  # hashed before compilation, including the two separately namespaced units.
  dep=run(flags+['-MM','-MT','probe',*units],'dependency-discovery',ROOT)
  md=run(flags+['-include','src/lib/crypto/namespace.h','-MM','-MT','probe',*mono],'crypto-dependency-discovery',ROOT)
  paths={os.path.normpath(p) for p in (dep+md).replace('\\\n',' ').split() if not p.endswith(':')}
  paths.update(('scripts/pkg_test_enroll.py','scripts/pkg_test_enroll_read.py','scripts/test_pkg_selector.py','tests/pkg_grant_candidate.mk'))
  sources={}
  for name in paths:
   p=ROOT/name;assert p.resolve().is_relative_to(ROOT);sources[name]=sha(p);target=work/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(p,target)
  m['sources']=sources;m['compiled_inputs']={p:sources[p] for p in paths if not p.endswith(('.py','.mk'))};save()
  objs=[]
  for i,p in enumerate(mono):
   obj=out/f'mono{i}.o';run(flags+['-include','src/lib/crypto/namespace.h','-c',p,'-o',str(obj)],f'compile-mono{i}');objs.append(str(obj))
  verifier=out/'verifier';run(flags+units+objs+['-o',str(verifier)],'compile')
  run(['gcc','-std=c11','-Os','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-fPIE','-fno-stack-protector','-mno-red-zone','-mno-sse','-mno-mmx','-msoft-float','-fstack-usage','-Wframe-larger-than=1024','-I.','-Itests','-Isrc/include','-Isrc/fs','-Iuser/tools','-c','tests/pkg_selector_candidate.c','-o',str(out/'selector-free.o')],'freestanding')
  prior=ROOT/'.codex-remote-attachments/pkg-kernel-backend-host-20261024-073146-531274';assert json.loads((prior/'manifest.json').read_text())['status']=='PASS'
  image=prior/'4096-512-setup.img';descriptor=prior/'4096-512-enrollment.bin';fixture_hashes={str(p.relative_to(ROOT)):sha(p) for p in (image,descriptor,prior/'fixture-inputs/old.bin',prior/'4096-512-policy.bin')};m['fixture_inputs']=fixture_hashes
  records=out/'records';records.mkdir();parts=[descriptor.read_bytes()]
  disk_names=('/pkg/enrollment.v1','/pkg/grant.v1','/pkg/ownership.v1','/pkg/account-generation.v1','/pkg/origin.v1','/pkg/state.v1','/pkg/trust.v1')
  for name,file in zip(disk_names,NAMES[1:]):
   dest=records/file;run(['debugfs','-R',f'dump {name} {dest}',str(image)],'extract-'+file);parts.append(dest.read_bytes())
  # Prepare canonical genesis record images independently of the producer.
  # The source fixture has a RETIRING demo; these prepared artifacts are NOT
  # written back to its filesystem or claimed to match its live namespace.
  def seal(b):b=bytearray(b);b[-32:]=H(b[:-32]);return bytes(b)
  state=(prior/'fixture-inputs/old.bin').read_bytes()
  table=bytearray(parts[3]);table[22:24]=bytes(2);table[32:48]=bytes(16);table[48:56]=(1).to_bytes(8,'little');table[256:8160]=bytes(7904);table=seal(table)
  ledger=bytearray(parts[2]);ledger[22:24]=bytes(2);ledger[24:32]=(1).to_bytes(8,'little');ledger[40:56]=bytes(16);ledger[136:168]=bytes(32);ledger[168:232]=H(state)*2;ledger[264:296]=H(table);ledger[296:344]=bytes(48);ledger[348:352]=bytes(4);ledger[360:376]=(1).to_bytes(8,'little')*2;ledger[392:432]=bytes(40);ledger=seal(ledger)
  origin=bytearray(parts[5]);origin[22:24]=bytes(2);origin[24:32]=(1).to_bytes(8,'little');origin[40:48]=(1).to_bytes(8,'little');origin[112:992]=bytes(880);origin=seal(origin)
  account=bytearray(parts[4]);account[20:24]=b'\x03\x00\x00\x00';account[24:40]=(1).to_bytes(8,'little')*2;account[112:144]=H(origin);account[440:448]=(1).to_bytes(8,'little');account[448:544]=H(ledger)+H(parts[7])+H(state);account[544:992]=bytes(448);account=seal(account)
  parts[2:7]=[ledger,table,account,origin,state]
  for name,data in zip(NAMES,parts):(records/name).write_bytes(data)
  (records/NAMES[0]).write_bytes(parts[0])
  for f in records.iterdir():f.chmod(0o600)
  en,marker,grant,owners,account,origin,state,trust=parts
  selkeys=[Ed25519PrivateKey.from_private_bytes(bytes([v])*32) for v in (111,112,113)]
  enkeys=[Ed25519PrivateKey.from_private_bytes(bytes([v])*32) for v in (41,42,43)]
  catkeys=[Ed25519PrivateKey.from_private_bytes(bytes([v])*32) for v in (1,2,3)]
  root_auth={'profile':'fortress-pkg2-test-only-v1','version':3,'repository':en[88:104].hex(),'roles':{name:{'threshold':2,'keys':[PUB(k).hex() for k in keys]} for name,keys in [('selector',selkeys),('enrollment',enkeys),('catalog',catkeys)]}}
  root_bytes=json.dumps(root_auth,sort_keys=True,separators=(',',':')).encode();(out/'root-authority.json').write_bytes(root_bytes)
  epoch=struct.unpack_from('<Q',en,24)[0];now=1792540800
  spec={'profile':'fortress-pkg2-test-only-v1','epoch':epoch,'issued':now-1,'expires':now+50,'uuid':[en[40+16*i:56+16*i].hex() for i in range(5)],'resource_profile':en[152:184].hex(),'platform':(prior/'4096-512-policy.bin').read_bytes()[720:752].hex(),'kernel':en[184:216].hex(),'bootstrap':en[216:248].hex(),'root_hash':H(root_bytes).hex(),'root_version':3,'catalog_hash':trust[64:96].hex(),'catalog_revision':7,'base_version':2,'selector_id':H(b'private-test-selector-1')[:16].hex(),'records':{n:str(records/n) for n in NAMES}}
  specpath=out/'spec.json';specpath.write_text(json.dumps(spec,indent=2)+'\n')
  # Independent oracle uses per-field integer encodings, rather than the
  # producer's struct packing/validation or the C parser.
  expected_bundle=bytearray(b'FORT-PKG-BND-v1\0'+(96+sum(map(len,parts))).to_bytes(4,'little')+b'\x01\x00\x08\x00'+bytes(8))
  for i,p in enumerate(parts):expected_bundle+=len(p).to_bytes(4,'little')+(i+1).to_bytes(2,'little')+bytes(2)
  expected_bundle+=b''.join(parts)
  body=bytearray(768);body[:16]=b'FORT-PKG-SEL-v1\0'
  for off,n,size in ((16,960,4),(20,1,2),(24,now-1,8),(32,now+50,8),(40,epoch,8),(48,2,4),(52,2,4),(56,3,8),(64,7,8),(72,2,8),(480,0x4c455354,4),(484,2,2),(488,1,4)):body[off:off+size]=n.to_bytes(size,'little')
  body[96:176]=en[40:120]
  for off,data in ((176,en[152:184]),(208,H(marker)),(240,H(en)),(272,H(expected_bundle)),(304,H(root_bytes)),(336,trust[64:96]),(368,bytes.fromhex(spec['platform'])),(400,en[184:216]),(432,en[216:248]),(464,bytes.fromhex(spec['selector_id']))):body[off:off+len(data)]=data
  def sign(b,keys=selkeys[:2]):
   b=bytearray(b);b[16:20]=(768+96*len(keys)).to_bytes(4,'little');b[484:486]=len(keys).to_bytes(2,'little');return bytes(b)+b''.join(k+sig for k,sig in sorted((H(PUB(key)),key.sign(bytes(b))) for key in keys))
  oracle=sign(body);public=b''.join(b''.join(PUB(k) for k in keys).ljust(256,b'\0') for keys in (selkeys,enkeys,catkeys))
  public+=struct.pack('<6I',3,2,3,2,3,2)+en[40:120]+en[152:184]+bytes.fromhex(spec['platform'])+en[184:248]+H(root_bytes)+trust[64:96]+H(oracle)+struct.pack('<5Q',now,epoch,3,7,2)
  assert len(public)==1136;policy=out/'trusted-policy.bin';policy.write_bytes(public)
  bundlepath=out/'oracle-bundle.bin';bundlepath.write_bytes(expected_bundle)
  counter=0
  def case(label,s=oracle,b=expected_bundle,p=public,ok=False):
   nonlocal counter
   counter+=1;prefix=out/f'vector-{counter:04d}';sf=prefix.with_suffix('.selector');bf=prefix.with_suffix('.bundle');pf=prefix.with_suffix('.policy')
   sf.write_bytes(s);bf.write_bytes(b);pf.write_bytes(p);run([str(verifier),str(pf),str(sf),str(bf),'1' if ok else '0'],f'vector-{counter:04d}');m['vectors'].append(dict(label=label,accepted=ok,selector_sha256=sha(sf),bundle_sha256=sha(bf)))
  def pin(s,p=public):p=bytearray(p);p[1064:1096]=H(s);return bytes(p)
  case('valid',ok=True)
  newer=bytearray(public);struct.pack_into('<Q',newer,1104,epoch-1);newer[1064:1096]=bytes(32);case('authenticated-above-selector-floor',p=newer,ok=True)
  for n in (0,1,16,767,768,959,961,1537):case('length-'+str(n),s=(oracle+b'x'*1537)[:n])
  def bundle_case(label,b):
   bad=bytearray(body);bad[272:304]=H(b);s=sign(bad);case(label,s=s,b=b,p=pin(s))
  for n in (0,1,31,95,len(expected_bundle)-1,len(expected_bundle)+1,32769):bundle_case('bundle-length-'+str(n),(expected_bundle+bytes(32769))[:n])
  for off in [0,16,20,22,*range(24,32)]:
   b=bytearray(expected_bundle);b[off]^=1;bundle_case('bundle-header-'+str(off),b)
  for i in range(8):
   for off,data in ((32+8*i,bytes(4)),(32+8*i,b'\xff'*4),(36+8*i,b'\xff\x00'),(38+8*i,b'\x01\x00')):
    b=bytearray(expected_bundle);b[off:off+len(data)]=data;bundle_case('bundle-row-'+str(i)+'-'+str(off)+'-'+data.hex(),b)
  for off in list(range(80,96))+list(range(492,768))+[22,23,486,487]:
   bad=bytearray(body);bad[off]=1;s=sign(bad);case('reserved-'+str(off),s=s,p=pin(s))
  for off in (20,24,32,40,48,52,56,64,72,96,112,128,144,160,176,208,240,272,304,336,368,400,432,464,480,488):
   bad=bytearray(body);bad[off]^=1
   if off in (24,32):bad[off:off+8]=(now+1 if off==24 else now).to_bytes(8,'little')
   s=sign(bad);case('field-'+str(off),s=s,p=pin(s),ok=off==464)
  for off in (768,800,831,850,863,864,896,927,946,959):
   bad=bytearray(oracle);bad[off]^=1;case('signature-byte-'+str(off),s=bad,p=pin(bad))
  for keys,label in ((selkeys[:1],'below-threshold'),(enkeys[:2],'enrollment-role'),(catkeys[:2],'catalog-role'),([selkeys[0],selkeys[0]],'duplicate-signer')):
   s=sign(body,keys);case(label,s=s,p=pin(s))
  s=sign(body,selkeys);case('three-valid-signers',s=s,p=pin(s),ok=True)
  bad=bytearray(oracle);bad[768:864],bad[864:960]=bad[864:960],bad[768:864];case('unsorted-signers',s=bad,p=pin(bad))
  for off,value in ((1096,now+50),(1096,now-2),(1104,epoch+1),(1112,4),(1120,8),(1128,3)):
   bad=bytearray(public);struct.pack_into('<Q',bad,off,value);case('policy-'+str(off)+'-'+str(value),p=bad)
  for off,value in ((768,0),(768,9),(772,0),(772,4),(776,0),(780,4),(784,0),(788,4)):
   bad=bytearray(public);struct.pack_into('<I',bad,off,value);case('policy-role-bound-'+str(off)+'-'+str(value),p=bad)
  bad=bytearray(public);bad[96]=1;case('unused-policy-key',p=bad)
  s=bytearray(body);s[464]^=1;s=sign(s);case('equal-epoch-substitution',s=s)
  live=bytearray(body);struct.pack_into('<Q',live,32,now+120);s=sign(live);late=bytearray(pin(s));struct.pack_into('<Q',late,1096,now+61);case('expired-delegated-catalog-with-live-selector',s=s,p=late)
  for off,value in ((24,0),(24,1767225599),(32,4102444801),(32,2**64-1)):
   bad=bytearray(body);struct.pack_into('<Q',bad,off,value);s=sign(bad);case('validity-bound-'+str(off)+'-'+str(value),s=s,p=pin(s))
  reused=bytearray(public);reused[256:512]=reused[:256];case('role-key-reuse',p=reused)
  for index in range(8):
   corrupted=bytearray(expected_bundle);at=96+sum(len(p) for p in parts[:index]);corrupted[at+20]^=1;bad=bytearray(body);bad[272:304]=H(corrupted);s=sign(bad);case('signed-mixed-record-'+str(index),s=s,b=corrupted,p=pin(s))
  for index,offset in ((2,22),(2,24),(3,22),(4,22),(4,24),(4,144),(4,152),(4,160),(4,162),(4,168),(4,424),(5,22),(6,112)):
   changed=list(parts);r=bytearray(changed[index]);r[offset]^=1;r[-32:]=H(r[:-32]);changed[index]=bytes(r);b=bytearray(expected_bundle[:96])+b''.join(changed);bad=bytearray(body);bad[272:304]=H(b);s=sign(bad);case('rehashed-initial-field-'+str(index)+'-'+str(offset),s=s,b=b,p=pin(s),ok=index==4 and offset==424)
  for i in range(3):
   for value in (0,8193):
    changed=list(parts);r=bytearray(changed[4]);struct.pack_into('<I',r,424+4*i,value);r[-32:]=H(r[:-32]);changed[4]=bytes(r);bundle_case('credential-witness-size-'+str(i)+'-'+str(value),bytearray(expected_bundle[:96])+b''.join(changed))
  changed=list(parts);r=bytearray(changed[4]);r[328:360]=bytes(32);r[-32:]=H(r[:-32]);changed[4]=bytes(r);bundle_case('zero-credential-content-witness',bytearray(expected_bundle[:96])+b''.join(changed))
  with tempfile.TemporaryDirectory(prefix='fortress-selector-keys-') as tmp:
   seeds=[]
   for i in (111,112):
    f=pathlib.Path(tmp)/str(i);f.write_bytes(bytes([i])*32);f.chmod(0o600);seeds.append(f)
   selector,bundle,audit=prepare(specpath,seeds);assert selector==oracle and bundle==expected_bundle
   for invalid in ('production-profile','ambiguous-UUID-widths','one-signer','duplicate-seed','repository-seed','world-readable-seed','short-seed'):
    chosen=seeds;chosen_spec=specpath
    if invalid=='production-profile':
     wrong=dict(spec);wrong['profile']='production';chosen_spec=pathlib.Path(tmp)/'wrong-spec.json';chosen_spec.write_text(json.dumps(wrong))
    if invalid=='ambiguous-UUID-widths':
     wrong=dict(spec);wrong['uuid']=[spec['uuid'][0]+spec['uuid'][1],'',*spec['uuid'][2:]];chosen_spec=pathlib.Path(tmp)/'wrong-width-spec.json';chosen_spec.write_text(json.dumps(wrong))
    if invalid=='one-signer':chosen=seeds[:1]
    if invalid=='duplicate-seed':chosen=[seeds[0],seeds[0]]
    if invalid=='repository-seed':chosen=[records/'marker.bin',seeds[1]]
    if invalid=='world-readable-seed':seeds[0].chmod(0o644)
    if invalid=='short-seed':
     short=pathlib.Path(tmp)/'short';short.write_bytes(bytes(31));short.chmod(0o600);chosen=[short,seeds[1]]
    try:prepare(chosen_spec,chosen)
    except ValueError:pass
    else:raise AssertionError('producer accepted '+invalid)
    seeds[0].chmod(0o600)
   m['producer_refusals']=7
   # POSIX permission and fsync checks run on Linux /tmp. Retained copies on
   # the Windows checkout preserve bytes, not the tested POSIX mode semantics.
   publication=pathlib.Path(tmp)/'artifacts';publication.mkdir();dest=publication/'published'
   result=publish(selector,bundle,audit,dest,verifier,policy);m['publication']=result
   assert inspect(dest,True,verifier,policy)=='PROTECTED' and inspect(dest,False,verifier,policy)=='REFUSE'
   assert inspect(out/'absent',False,verifier,policy)=='LEGACY' and inspect(out/'absent',True,verifier,policy)=='REFUSE'
   for cut in range(1,len(result['events'])+1):
    target=publication/f'cut-{cut:02d}'
    try:publish(selector,bundle,audit,target,verifier,policy,cut)
    except InterruptedError:pass
    else:raise AssertionError('missing cut')
    admitted=inspect(target,True,verifier,policy);assert admitted==('PROTECTED' if cut>=result['events'].index('selector-renamed')+1 else 'REFUSE')
    assert inspect(target,False,verifier,policy)=='REFUSE';m['cuts'].append(dict(cut=cut,event=result['events'][cut-1],admission=admitted))
   try:publish(selector,bundle,audit,dest,verifier,policy)
   except ValueError:pass
   else:raise AssertionError('existing output overwritten')
   for kind in ('missing-root','missing-selector','temporary-selector','wrong-mode','symlink','directory'):
    target=publication/kind;shutil.copytree(dest,target)
    if kind=='missing-root':(target/'root/records.bin').unlink()
    if kind=='missing-selector':(target/'boot/selector.bin').unlink()
    if kind=='temporary-selector':(target/'boot/selector.bin').rename(target/'boot/selector.tmp')
    if kind=='wrong-mode':(target/'root/records.bin').chmod(0o644)
    if kind=='symlink':(target/'root/records.bin').unlink();(target/'root/records.bin').symlink_to(dest/'root/records.bin')
    if kind=='directory':(target/'root/records.bin').unlink();(target/'root/records.bin').mkdir()
    assert inspect(target,True,verifier,policy)=='REFUSE' and inspect(target,False,verifier,policy)=='REFUSE'
   import unittest.mock,errno
   with unittest.mock.patch('pkg_test_enroll_read.os.open',side_effect=OSError(errno.EIO,'injected read failure')):
    assert inspect(dest,True,verifier,policy)=='REFUSE' and inspect(dest,False,verifier,policy)=='REFUSE'
   m['publication_files']={str(p.relative_to(publication)):{'sha256':sha(p),'mode':oct(p.stat().st_mode&0o777),'bytes':p.stat().st_size} for p in publication.rglob('*') if p.is_file() and not p.is_symlink()}
   shutil.copytree(publication,out/'retained-publication-bytes',symlinks=True)
  assert all(sha(ROOT/p)==h for p,h in fixture_hashes.items());m['result']='PASS'
 except BaseException as e:m['result']='FAIL';m['error']=repr(e);raise
 finally:
  m['changed_compiled_copies']=[p for p,h in m.get('compiled_inputs',{}).items() if sha(work/p)!=h]
  m['changed_sources']=[p for p,h in m.get('sources',{}).items() if sha(ROOT/p)!=h]
  m['changed_guards']=[p for p,h in guards.items() if sha(ROOT/p)!=h]
  if any(m[k] for k in ('changed_compiled_copies','changed_sources','changed_guards')):m['preservation']='FAIL'
  else:m['preservation']='PASS'
  save();print('Evidence:',out,m['result'],m['preservation'],flush=True)
 assert m['preservation']=='PASS'
if __name__=='__main__':main()
