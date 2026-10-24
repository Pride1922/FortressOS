"""Offline private TEST enrollment artifact producer. Never opens a disk.

Requires external test seed files and an independently trusted reader policy.
Publishes a prepared record bundle before its signed boot selector. No existing
output is replaced; partial outputs are retained on error/interruption.
"""
import argparse,hashlib,json,os,pathlib,stat,struct,subprocess,tempfile
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding,PublicFormat
ROOT=pathlib.Path(__file__).resolve().parents[1]
H=lambda b:hashlib.sha256(b).digest()
NAMES=('enrollment.bin','marker.bin','grant.bin','owners.bin','account.bin','origin.bin','state.bin','trust.bin')

def regular(path,cap):
 path=pathlib.Path(path)
 if path.is_symlink() or not stat.S_ISREG(path.stat().st_mode) or path.stat().st_size>cap:raise ValueError('bounded regular file required: '+str(path))
 return path.read_bytes()

def prepare(spec_path,seeds):
 spec=json.loads(regular(spec_path,16384));expected={'profile','epoch','issued','expires','uuid','resource_profile','platform','kernel','bootstrap','root_hash','root_version','catalog_hash','catalog_revision','base_version','selector_id','records'}
 if set(spec)!=expected or spec['profile']!='fortress-pkg2-test-only-v1':raise ValueError('exact private TEST specification required')
 def number(name):
  n=spec[name]
  if isinstance(n,bool) or not isinstance(n,int) or not 0<n<2**64-1:raise ValueError('invalid '+name)
  return n
 def value(name,size):
  b=bytes.fromhex(spec[name])
  if len(b)!=size or not any(b):raise ValueError('invalid '+name)
  return b
 if not 1767225600<=number('issued')<number('expires')<=4102444800:raise ValueError('invalid validity interval')
 if not isinstance(spec['uuid'],list) or len(spec['uuid'])!=5:raise ValueError('five UUIDs required')
 uuid_parts=[bytes.fromhex(x) for x in spec['uuid']]
 if any(len(x)!=16 or not any(x) for x in uuid_parts):raise ValueError('invalid UUIDs')
 uuids=b''.join(uuid_parts)
 if not isinstance(spec['records'],dict) or set(spec['records'])!=set(NAMES):raise ValueError('exact record set required')
 parts=[regular(spec['records'][n],3232 if i==0 else 8192) for i,n in enumerate(NAMES)]
 if any(len(p)!=n for p,n in zip(parts[1:],(512,1024,8192,1024,1024,1024,1024))):raise ValueError('record size')
 # Independent producer preflight. Full signatures/semantics are checked again
 # by the separately compiled C reader before either output is published.
 en,m,g,t,a,o,s,tr=parts
 if len(en)<2464 or en[:16]!=b'FORT-PKG-ENR-v2\0' or en[40:120]!=uuids or struct.unpack_from('<Q',en,24)[0]!=number('epoch'):raise ValueError('enrollment identity')
 for b in (m,g,t,a,o,s,tr):
  if H(b[:-32])!=b[-32:]:raise ValueError('record checksum')
 if struct.unpack_from('<HQ',g,22)!=(0,1) or struct.unpack_from('<H',t,22)[0] or any(g[296:344]) or any(g[40:56]):raise ValueError('initial IDLE required')
 if struct.unpack_from('<HQQ',a,22)!=(0,1,1) or struct.unpack_from('<H',o,22)[0] or struct.unpack_from('<Q',s,24)[0]!=1 or any(s[112:226]):raise ValueError('initial account/state required')
 if struct.unpack_from('<Q',a,144)[0]!=1 or any(a[152:328]) or any(not any(a[328+32*i:360+32*i]) or not 1<=struct.unpack_from('<I',a,424+4*i)[0]<=8192 for i in range(3)):raise ValueError('initial credential lineage required')
 bundle=bytearray(96);bundle[:16]=b'FORT-PKG-BND-v1\0';struct.pack_into('<IHH',bundle,16,96+sum(map(len,parts)),1,8)
 for i,p in enumerate(parts):struct.pack_into('<IHH',bundle,32+8*i,len(p),i+1,0);bundle.extend(p)
 body=bytearray(768);body[:16]=b'FORT-PKG-SEL-v1\0';struct.pack_into('<IHHQQQIIQQQ',body,16,768+96*len(seeds),1,0,number('issued'),number('expires'),number('epoch'),2,2,number('root_version'),number('catalog_revision'),number('base_version'))
 body[96:176]=uuids
 fields={'resource_profile':176,'root_hash':304,'catalog_hash':336,'platform':368,'kernel':400,'bootstrap':432}
 for name,offset in fields.items():body[offset:offset+32]=value(name,32)
 body[208:240]=H(m);body[240:272]=H(en);body[272:304]=H(bundle);body[464:480]=value('selector_id',16)
 struct.pack_into('<IHHI',body,480,0x4c455354,len(seeds),0,1)
 if not 2<=len(seeds)<=8:raise ValueError('TEST selector requires 2..8 external seed files')
 keys=[]
 for path in seeds:
  path=pathlib.Path(path).resolve()
  if path.is_relative_to(ROOT) or path.stat().st_mode&0o077:raise ValueError('seeds must be external private regular files')
  seed=regular(path,32)
  if len(seed)!=32:raise ValueError('32-byte test seed required')
  key=Ed25519PrivateKey.from_private_bytes(seed);pub=key.public_key().public_bytes(Encoding.Raw,PublicFormat.Raw)
  if any(pub==p for p,_ in keys):raise ValueError('duplicate signer')
  keys.append((pub,key))
 rows=sorted((H(pub),key.sign(bytes(body))) for pub,key in keys)
 selector=bytes(body)+b''.join(k+sig for k,sig in rows)
 return selector,bytes(bundle),{'test_only':True,'selector_sha256':H(selector).hex(),'bundle_sha256':H(bundle).hex(),'signer_fingerprints':sorted(H(pub).hex() for pub,_ in keys),'spec_sha256':H(regular(spec_path,16384)).hex()}

def publish(selector,bundle,audit,output,verifier,policy,cut=None):
 output=pathlib.Path(output).resolve();verifier=pathlib.Path(verifier).resolve();policy=pathlib.Path(policy).resolve()
 if output.exists() or not output.parent.is_dir():raise ValueError('new output directory required')
 # Verification takes place before creating any output. Inputs are ordinary
 # immutable files in a temporary host directory, never a mounted target root.
 def verify(s,b):
  with tempfile.TemporaryDirectory(prefix='fortress-enroll-check-') as d:
   d=pathlib.Path(d);(d/'selector').write_bytes(s);(d/'bundle').write_bytes(b)
   r=subprocess.run([str(verifier),str(policy),str(d/'selector'),str(d/'bundle'),'1'],capture_output=True,text=True,timeout=30)
   if r.returncode:raise ValueError('independent reader refused prepared enrollment')
 verify(selector,bundle);events=[]
 def event(name):
  events.append(name)
  if cut is not None and len(events)==cut:raise InterruptedError(name)
 def syncdir(path):
  fd=os.open(path,os.O_RDONLY|os.O_DIRECTORY)
  try:os.fsync(fd)
  finally:os.close(fd)
 output.mkdir(mode=0o700);(output/'root').mkdir(mode=0o700);(output/'boot').mkdir(mode=0o700);syncdir(output);event('directories-synced')
 for directory in (output,output/'root',output/'boot'):
  if stat.S_IMODE(directory.stat().st_mode)!=0o700:raise ValueError('output filesystem must enforce POSIX private modes')
 syncdir(output.parent);event('parent-directory-synced')
 def write(path,data,label):
  with path.open('xb') as f:
   os.chmod(path,0o600)
   if stat.S_IMODE(os.fstat(f.fileno()).st_mode)!=0o600:raise ValueError('output filesystem must enforce POSIX private modes')
   event(label+'-created');f.write(data);f.flush();event(label+'-written');os.fsync(f.fileno());event(label+'-fsynced')
 write(output/'root/records.bin',bundle,'root');syncdir(output/'root');event('root-directory-synced')
 reread=regular(output/'root/records.bin',32768)
 if reread!=bundle:raise ValueError('root readback mismatch')
 verify(selector,reread);event('root-readback-verified')
 write(output/'boot/selector.tmp',selector,'selector');syncdir(output/'boot');event('selector-temp-directory-synced')
 verify(regular(output/'boot/selector.tmp',1536),reread);event('selector-readback-verified')
 # Commit last, never overwrite. The new private directory is exclusively
 # owned; production multi-writer/cross-device enrollment is not implemented.
 if (output/'boot/selector.bin').exists():raise ValueError('selector already present')
 os.rename(output/'boot/selector.tmp',output/'boot/selector.bin');event('selector-renamed')
 syncdir(output/'boot');event('selector-directory-synced');syncdir(output);event('publication-directory-synced')
 return dict(audit,events=events)

def main():
 p=argparse.ArgumentParser();p.add_argument('--spec',required=True);p.add_argument('--seed',action='append',required=True);p.add_argument('--output',required=True);p.add_argument('--verifier',required=True);p.add_argument('--policy',required=True);p.add_argument('--cut',type=int);a=p.parse_args()
 selector,bundle,audit=prepare(a.spec,a.seed)
 try:result=publish(selector,bundle,audit,a.output,a.verifier,a.policy,a.cut)
 except InterruptedError as e:print('TEST INTERRUPTION:',str(e));return 75
 print(json.dumps(result,indent=2));return 0
if __name__=='__main__':raise SystemExit(main())
