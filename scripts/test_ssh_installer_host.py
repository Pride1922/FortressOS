#!/usr/bin/env python3
"""Actual installer v4 host-key handoff; published deterministic test vector only."""
from pathlib import Path
import ctypes as C
import json
import os
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parent.parent
def main():
    lib=C.CDLL(str(ROOT/'build/ssh-hostkeys-host/codec.so'))
    lib.crypto_ed25519_from_seed.argtypes=[C.c_void_p]*3
    lib.ssh_hostkey_encode.argtypes=[C.c_void_p,C.c_uint32,C.c_void_p,C.c_size_t,C.c_void_p,C.c_void_p]
    w=C.create_string_buffer(1952);pk=C.create_string_buffer(32);sk=C.create_string_buffer(64)
    encoded=C.create_string_buffer(1024);n=C.c_size_t()
    assert not lib.crypto_ed25519_from_seed(bytes(range(32)),pk,sk)
    assert not lib.ssh_hostkey_encode(sk,123,encoded,1024,C.byref(n),w)
    with tempfile.TemporaryDirectory(prefix='fortress-ssh-install-vector-') as tmp:
        key=Path(tmp)/'published-test-vector';key.write_bytes(encoded.raw[:n.value]);key.chmod(0o600)
        env=dict(os.environ,SSH_HOSTKEY_FIXTURE=str(key))
        result=subprocess.run(['python3','scripts/test_install_runtime.py','--username','operator','--hostname','ssh-fixture'],
                              cwd=ROOT,env=env,check=True,capture_output=True,text=True)
        out=Path(result.stdout.strip().splitlines()[-1]);assert out.is_relative_to(ROOT/'build/root-phase4')
        assert json.loads((out/'manifest.json').read_text())['result']=='PASS'
        loaded=Path(tmp)/'readback'
        subprocess.run(['debugfs','-R',f'dump /etc/ssh/ssh_host_ed25519_key {loaded}',str(out/'root.img')],check=True,capture_output=True)
        assert loaded.read_bytes()==key.read_bytes();loaded.chmod(0o600)
        public=subprocess.check_output(['ssh-keygen','-y','-f',str(loaded)])
        assert public.split()[0]==b'ssh-ed25519'
        metadata=subprocess.check_output(['debugfs','-R','stat /etc/ssh/ssh_host_ed25519_key',str(out/'root.img')],stderr=subprocess.DEVNULL,text=True)
        assert 'Mode:  0600' in metadata and 'User:     0' in metadata and 'Group:     0' in metadata
        print('PASS installer v4: target-only key, fresh-mount byte/metadata readback, independent OpenSSH parse; '+str(out))
if __name__=='__main__':main()
