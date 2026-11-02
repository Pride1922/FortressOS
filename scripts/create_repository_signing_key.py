"""Create the server's production Ed25519 key; retain an existing valid key.

Run under sudo on the repository server. Only the public key is printed.
"""
import os, stat
from pathlib import Path
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PrivateFormat, PublicFormat, NoEncryption
os.umask(0o077)
d=Path('/etc/pkg')
if d.is_symlink(): raise SystemExit('Refusing symlink /etc/pkg')
d.mkdir(mode=0o700,exist_ok=True)
s=d.stat()
if s.st_uid!=0 or s.st_mode & 0o022: raise SystemExit('Unsafe /etc/pkg ownership/permissions')
p=d/'signing.seed'
if p.exists() or p.is_symlink():
 s=p.lstat()
 if not stat.S_ISREG(s.st_mode) or s.st_uid!=0 or s.st_mode&0o077 or s.st_nlink!=1: raise SystemExit('Unsafe existing key')
 seed=p.read_bytes()
 if len(seed)!=32 or seed==bytes(range(32)): raise SystemExit('Invalid existing production key')
 key=Ed25519PrivateKey.from_private_bytes(seed)
else:
 key=Ed25519PrivateKey.generate()
 fd=os.open(p,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
 with os.fdopen(fd,'wb') as f:
  f.write(key.private_bytes(Encoding.Raw,PrivateFormat.Raw,NoEncryption()));f.flush();os.fsync(f.fileno())
public=key.public_key().public_bytes(Encoding.Raw,PublicFormat.Raw)
pub=d/'trust.pub'
if pub.exists() or pub.is_symlink():
 if pub.is_symlink() or pub.read_bytes()!=public: raise SystemExit('Existing public key differs')
else:
 fd=os.open(pub,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o644)
 with os.fdopen(fd,'wb') as f:f.write(public);f.flush();os.fsync(f.fileno())
os.chmod(pub,0o644)
print('Production public key:',public.hex())
print('Private seed retained only at /etc/pkg/signing.seed (root, 0600).')
