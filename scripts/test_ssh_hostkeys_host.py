#!/usr/bin/env python3
"""Actual freestanding codec, sanitizers and independent OpenSSH interoperability.
Generated secrets live only in a temporary directory and never enter logs.
"""
from pathlib import Path
import base64
import ctypes as C
import hashlib
import json
import os
import struct
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parent.parent
BUILD=ROOT/'build/ssh-hostkeys-host'

def run(args,**kw):
    return subprocess.run(list(map(str,args)),cwd=ROOT,check=True,**kw)

def main():
    BUILD.mkdir(parents=True,exist_ok=True)
    common=['gcc','-std=c11','-Wall','-Wextra','-Werror','-Isrc/include',
            '-Isrc/lib/crypto/upstream/src','-DCRYPTO_HOST_TEST']
    wrappers=[f'src/lib/crypto/{x}.c' for x in ('hash','curve','aead','entropy','ssh_hostkey')]
    vendor=['src/lib/crypto/upstream/src/monocypher.c',
            'src/lib/crypto/upstream/src/optional/monocypher-ed25519.c']
    for suffix,flags in [('san',['-O1','-g','-no-pie','-fsanitize=address,undefined','-fno-omit-frame-pointer']),
                         ('pic',['-O2','-fPIC'])]:
        objs=[]
        for i,p in enumerate(vendor):
            obj=BUILD/f'{suffix}-{i}.o';objs.append(obj)
            run(common+flags+['-include','src/lib/crypto/namespace.h','-c',p,'-o',obj])
        if suffix=='san':
            exe=BUILD/'hostkey-test'
            run(common+flags+['tests/ssh_hostkey_host.c',*wrappers,'src/lib/root_digest.c',*objs,'-o',exe])
            run([exe])
        else:
            run(common+flags+['-shared','tests/crypto_diff_shim.c',*wrappers,'src/lib/root_digest.c',*objs,'-o',BUILD/'codec.so'])
    lib=C.CDLL(str(BUILD/'codec.so'))
    ptr=C.c_void_p;size=C.c_size_t
    lib.crypto_ed25519_from_seed.argtypes=[ptr,ptr,ptr]
    lib.ssh_hostkey_encode.argtypes=[ptr,C.c_uint32,ptr,size,ptr,ptr]
    lib.ssh_hostkey_decode.argtypes=[ptr,size,ptr,ptr]
    for name in ('ssh_hostkey_public','ssh_hostkey_fingerprint'):
        getattr(lib,name).argtypes=[ptr,ptr,size,ptr,ptr]
    w=C.create_string_buffer(1952) # header sizeof checked by sanitizer C test
    out=C.create_string_buffer(1024);pk=C.create_string_buffer(32);sk=C.create_string_buffer(64)
    decoded=C.create_string_buffer(64);n=size()
    assert lib.crypto_ed25519_from_seed(bytes(range(32)),pk,sk)==0
    assert lib.ssh_hostkey_encode(sk,123,out,1024,C.byref(n),w)==0
    encoded=out.raw[:n.value]
    assert lib.ssh_hostkey_public(pk,out,1024,C.byref(n),w)==0
    public=out.raw[:n.value]
    assert lib.ssh_hostkey_fingerprint(pk,out,1024,C.byref(n),w)==0
    fingerprint=out.raw[:n.value].decode()
    expected='SHA256:'+base64.b64encode(hashlib.sha256(base64.b64decode(public.split()[1])).digest()).decode().rstrip('=')
    assert fingerprint==expected
    cases=0
    def decode(data,valid,secret=None):
        nonlocal cases
        C.memset(decoded,0x5a,64)
        r=lib.ssh_hostkey_decode(data,len(data),decoded,w)
        assert (r==0)==valid
        if not valid: assert decoded.raw==b'Z'*64
        elif secret is not None: assert decoded.raw==secret
        assert not any(w.raw)
        cases+=1
    def armor(raw):
        b=base64.b64encode(raw)
        return b'-----BEGIN OPENSSH PRIVATE KEY-----\n'+b'\n'.join(b[i:i+70] for i in range(0,len(b),70))+b'\n-----END OPENSSH PRIVATE KEY-----\n'
    raw=base64.b64decode(b''.join(encoded.splitlines()[1:-1]))
    decode(encoded,True,sk.raw);decode(encoded.replace(b'\n',b'\r\n'),True,sk.raw)
    # Structural corruption at every decoded byte, including seed and repeated
    # public copies. Checkints may change together; comments may change legally.
    for i in range(len(raw)):
        v=bytearray(raw);v[i]^=128
        decode(armor(v),False)
    for bad in (encoded+b'\n',encoded.replace(b'\n',b'\x00',1),b'x'*1025,armor(raw+b'\0')):
        decode(bad,False)
    with tempfile.TemporaryDirectory(prefix='fortress-hostkey-') as temp:
        temp=Path(temp);key=temp/'key';key.write_bytes(encoded);key.chmod(0o600)
        imported=run(['ssh-keygen','-y','-f',key],capture_output=True).stdout.split()
        assert imported[:2]==public.split()[:2]
        pub=temp/'key.pub';pub.write_bytes(public)
        assert fingerprint in run(['ssh-keygen','-lf',pub],capture_output=True).stdout.decode()
        # OpenSSH generated comments exercise every alignment, including zero
        # padding, empty comment and the maximum accepted comment size.
        for comment in [*map(lambda x:'x'*x,range(9)),'x'*64]:
            generated=temp/'generated'
            run(['ssh-keygen','-q','-t','ed25519','-N','','-C',comment,'-f',generated],capture_output=True)
            data=generated.read_bytes();decode(data,True)
            expected=run(['ssh-keygen','-y','-f',generated],capture_output=True).stdout.split()[:2]
            assert lib.ssh_hostkey_public(C.byref(decoded,32),out,1024,C.byref(n),w)==0
            assert out.raw[:n.value].split()[:2]==expected
            generated.unlink();Path(str(generated)+'.pub').unlink()
    run(['make','-j8','build/crypto/libfortresscrypto.a'])
    frames={}
    for line in (ROOT/'build/crypto/ssh_hostkey.su').read_text().splitlines():
        name,count,kind=line.split('\t');assert kind in ('static','dynamic,bounded')
        frames[name.split(':')[-1]]=int(count);assert int(count)<1024
    report={'status':'PASS','independent_cases':cases,'frames':frames,
            'openssh':run(['ssh','-V'],capture_output=True).stderr.decode().strip()}
    (BUILD/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print(f'PASS OpenSSH codec interoperability and {cases} independent cases; {BUILD / "result.json"}')

if __name__=='__main__':main()
