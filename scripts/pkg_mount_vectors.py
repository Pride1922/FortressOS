"""Independent Python fixtures; public TEST catalog keys, no provisioning."""
import hashlib,struct,pathlib
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding,PublicFormat
from pkg_artifact_vectors import elf,tar_header,pack
from pkg_manifest_vectors import text
NOW=1792540800
PROFILE=hashlib.sha256(b'Fortress bounded mount profile v1').digest()
def h(b):return hashlib.sha256(b).digest()
def seal(b):b=bytearray(b);b[992:]=h(b[:992]);return bytes(b)
def header(domain):
 b=bytearray(1024);b[:16]=domain.ljust(16,b'\0');struct.pack_into('<IHH',b,16,1024,1,0);return b

def vectors(out,bad_signature=False,with_doc=False,foreign_genesis=False):
 out=pathlib.Path(out);out.mkdir(parents=True,exist_ok=True);executable=elf();repo=bytes([4])*16
 manifest=struct.pack('<4sHHQQQ',b'FMN1',1,2 if with_doc else 1,1,1,3)+bytes(range(32))+text('demo')+text('demo')+text('1')+text('')
 manifest+=struct.pack('<BBHQ',1,0,0o755,len(executable))+h(executable)+text('bin/demo')
 tar=tar_header('bin/demo',executable,0o755)+executable+bytes((-len(executable))%512)
 if with_doc:
  doc=b'valid signed documentation\n';manifest+=struct.pack('<BBHQ',2,0,0o644,len(doc))+h(doc)+text('doc/readme');tar+=tar_header('doc/readme',doc,0o644)+doc+bytes((-len(doc))%512)
 tar+=bytes(1024)
 artifact=pack(manifest,tar)
 keys=[Ed25519PrivateKey.from_private_bytes(bytes([i])*32) for i in (1,2,3)];pub=[k.public_key().public_bytes(Encoding.Raw,PublicFormat.Raw) for k in keys]
 entry=bytes([4])+b'demo'.ljust(32,b'\0')+struct.pack('<QQ',1,len(artifact))+h(artifact)
 body=struct.pack('<16s16sQQQQHHI',b'FORT-PKG-CAT-v1\0',repo,3,7,NOW-2,NOW+60,1,2,0)+entry
 catalog=body+b''.join(i+s for i,s in sorted((h(pub[n]),keys[n].sign(body)) for n in (0,1)))
 if bad_signature:catalog=catalog[:-1]+bytes([catalog[-1]^1])
 trust=header(b'FORT-PKG-TR-v1');struct.pack_into('<QQQ',trust,24,3,7,NOW-1);trust[48:64]=repo;trust[64:96]=h(catalog);struct.pack_into('<I',trust,96,len(catalog));trust[256:256+len(catalog)]=catalog;trust=seal(trust)
 old=header(b'FORT-PKG-ST-v1');struct.pack_into('<Q',old,24,1);old[32:48]=repo;old[48:80]=h(trust);old=seal(old)
 if foreign_genesis:
  old=bytearray(old);struct.pack_into('<Q',old,24,7);old[80:112]=h(b'unrecognized history');old=seal(old)
 new=bytearray(old);struct.pack_into('<Q',new,24,2);new[80:112]=h(old);struct.pack_into('<Q',new,112,1);new[120:152]=h(artifact);new[152:184]=h(executable);struct.pack_into('<Q',new,184,len(executable));new[192]=4;new[193:197]=b'demo';new=seal(new)
 plan=h(h(new)+h(artifact)+PROFILE)
 pending=header(b'FORT-PKG-PN-v1');struct.pack_into('<H',pending,22,1);pending[24:40]=bytes([0xa1])*16;pending[40:56]=bytes([5])*16;pending[56:72]=repo
 pending[72:104],pending[104:136],pending[136:168],pending[168:200],pending[200:232]=h(old),h(new),h(trust),h(artifact),plan
 struct.pack_into('<QQQQQQQ',pending,232,1,2,3,7,NOW,len(artifact),1);pending[288]=4;pending[289:293]=b'demo';pending[328:360]=h(executable);struct.pack_into('<Q',pending,360,len(executable));pending=seal(pending)
 rules=struct.pack('<16sQQQ32sII256s',repo,3,7,NOW,h(catalog),2,3,b''.join(pub).ljust(256,b'\0'))+bytes(range(32))+struct.pack('<Q',2)
 for name,b in [('artifact.fpk',artifact),('executable.bin',executable),('trust.bin',trust),('old.bin',old),('next.bin',new),('pending.bin',pending),('policy-extra.bin',rules),('base.bin',bytes(range(32))+struct.pack('<Q',2))]:(out/name).write_bytes(b)
 return rules
