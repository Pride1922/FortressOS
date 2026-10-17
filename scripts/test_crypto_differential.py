#!/usr/bin/env python3
"""Independent behavioral comparison with Python hashlib and cryptography.
Fixed public test inputs only; no OpenSSL/BoringSSL vector files imported.
"""
import ctypes as c
import hashlib
import hmac
from pathlib import Path
import random
import sys
from cryptography.hazmat.primitives.asymmetric import ed25519,x25519
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.serialization import Encoding,PublicFormat

lib=c.CDLL(str(Path(sys.argv[1]).resolve()))
P=c.c_void_p;N=c.c_size_t
def setup(name,types):
    f=getattr(lib,name);f.argtypes=types;f.restype=c.c_int;return f
sha256=setup('crypto_sha256',[P,N,P]);sha512=setup('crypto_sha512',[P,N,P])
hm=setup('crypto_hmac_sha256',[P,N,P,N,P])
ed=setup('crypto_ed25519_from_seed',[P,P,P]);sign=setup('crypto_ed25519_sign',[P,P,P,N]);verify=setup('crypto_ed25519_verify',[P,P,P,N])
x=setup('crypto_x25519_from_seed',[P,P,P]);dh=setup('crypto_x25519',[P,P,P])
seal=setup('crypto_aead_lock',[P,P,P,P,P,N,P,N]);unseal=setup('crypto_aead_unlock',[P,P,P,P,P,N,P,N])
r=random.Random(0x46525432)
def data(n): return r.randbytes(n)
def out(n): return c.create_string_buffer(n)
def rawpub(key): return key.public_key().public_bytes(Encoding.Raw,PublicFormat.Raw)
sizes=[0,1,15,16,17,31,32,33,55,56,63,64,65,111,112,127,128,129,255,256,1023,1024,4096,65536]
for i in range(96):
    n=sizes[i%len(sizes)];msg=data(n);key=data([0,1,32,63,64,65,131,256][i%8]);seed=data(32)
    a=out(32);b=out(64)
    assert sha256(msg,n,a)==0 and a.raw==hashlib.sha256(msg).digest()
    assert sha512(msg,n,b)==0 and b.raw==hashlib.sha512(msg).digest()
    assert hm(key,len(key),msg,n,a)==0 and a.raw==hmac.new(key,msg,hashlib.sha256).digest()
    sk=out(64);pk=out(32);sig=out(64)
    assert ed(seed,pk,sk)==0
    ref=ed25519.Ed25519PrivateKey.from_private_bytes(seed)
    assert pk.raw==rawpub(ref)
    assert sign(sig,sk,msg,n)==0 and sig.raw==ref.sign(msg)
    assert verify(sig,pk,msg,n)==0
    broken=bytes([sig.raw[0]^1])+sig.raw[1:]
    assert verify(broken,pk,msg,n)<0
    xsk=out(32);xpk=out(32);peer=data(32);shared=out(32)
    assert x(seed,xsk,xpk)==0
    xr=x25519.X25519PrivateKey.from_private_bytes(seed)
    pr=x25519.X25519PrivateKey.from_private_bytes(peer)
    assert xpk.raw==rawpub(xr)
    assert dh(shared,xsk,rawpub(pr))==0 and shared.raw==xr.exchange(pr.public_key())
    key=data(32);nonce=data(12);ad=data(i%37);ct=out(n);tag=out(16);plain=out(n)
    expected=ChaCha20Poly1305(key).encrypt(nonce,msg,ad)
    assert seal(ct,tag,key,nonce,ad,len(ad),msg,n)==0
    assert ct.raw+tag.raw==expected
    assert unseal(plain,tag,key,nonce,ad,len(ad),ct,n)==0 and plain.raw==msg
    # Independent malformed authenticated records and no failure publication.
    bad=bytes([tag.raw[0]^1])+tag.raw[1:];c.memset(plain,0xa5,n)
    assert unseal(plain,bad,key,nonce,ad,len(ad),ct,n)<0
    assert plain.raw==bytes([0xa5])*n
msg=b'a'*(1024*1024)
for f,ref,size in [(sha256,hashlib.sha256,32),(sha512,hashlib.sha512,64)]:
    b=out(size);assert f(msg,len(msg),b)==0 and b.raw==ref(msg).digest()
print('PASS 96 independent differential cases across all primitives; 1 MiB hashes; tampered AEAD preserves output')
