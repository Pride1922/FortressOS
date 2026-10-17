#!/usr/bin/env python3
"""Host ASan/UBSan test runner for SSH transport layer, sshbuf, ChaCha20-Poly1305, and KEX."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / 'build/ssh-transport-host'

def run(args, **kw):
    return subprocess.run(list(map(str, args)), cwd=ROOT, check=True, **kw)

def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    common = [
        'gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-Isrc/include', '-Isrc/lib/crypto/upstream/src', '-I.',
        '-DCRYPTO_HOST_TEST'
    ]
    crypto_wrappers = [f'src/lib/crypto/{x}.c' for x in ('hash', 'curve', 'aead', 'entropy')]
    vendor = [
        'src/lib/crypto/upstream/src/monocypher.c',
        'src/lib/crypto/upstream/src/optional/monocypher-ed25519.c'
    ]
    sshd_units = ['user/sshd/sshbuf.c', 'user/sshd/cipher.c', 'user/sshd/kex.c', 'user/sshd/transport.c']

    flags = ['-O1', '-g', '-no-pie', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']

    objs = []
    for i, p in enumerate(vendor):
        obj = BUILD / f'vendor-{i}.o'
        objs.append(obj)
        if not obj.exists():
            run(common + flags + ['-include', 'src/lib/crypto/namespace.h', '-c', p, '-o', obj])

    exe = BUILD / 'transport-test'
    run(common + flags + [
        'tests/ssh_transport_host.c',
        *sshd_units,
        *crypto_wrappers,
        'src/lib/root_digest.c',
        *objs,
        '-o', exe
    ])

    print(f"+ {exe}")
    res = subprocess.run([str(exe)], cwd=ROOT)
    if res.returncode != 0:
        print(f"FAILED: transport-test exited with {res.returncode}", file=sys.stderr)
        sys.exit(res.returncode)

    print("PASS: SSH transport engine, sshbuf, chacha20-poly1305, and KEX gates verified under ASan/UBSan")

    # Include the actual daemon receive helper; discard unrelated daemon entry
    # points so this adapter only mocks the socket syscall, not packet handling.
    worker_exe = BUILD / 'worker-receive-test'
    run(common + flags + ['-Iuser/tools', '-Isrc/fs',
        '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
        'tests/ssh_worker_receive_host.c', *sshd_units, 'user/sshd/auth.c',
        *crypto_wrappers, 'src/lib/root_digest.c', *objs, '-o', worker_exe])
    run([worker_exe])

    # Run live OpenSSH client interoperability test if ssh is installed
    test_live_openssh_interop()

def test_live_openssh_interop():
    import socket, struct, hashlib, shutil
    if not shutil.which('ssh'):
        print("SKIP: /usr/bin/ssh not found; skipping live OpenSSH client test")
        return

    try:
        from cryptography.hazmat.primitives.asymmetric import x25519, ed25519
        from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat
    except ImportError:
        print("SKIP: python cryptography library not found; skipping live OpenSSH oracle")
        return

    print("Running live OpenSSH client interoperability oracle...")
    host_priv = ed25519.Ed25519PrivateKey.generate()
    host_pub_bytes = host_priv.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
    ks_blob = struct.pack('>I', 11) + b'ssh-ed25519' + struct.pack('>I', 32) + host_pub_bytes

    server_eph_priv = x25519.X25519PrivateKey.generate()
    server_eph_pub_bytes = server_eph_priv.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(('127.0.0.1', 0))
    s.listen(1)
    port = s.getsockname()[1]
    V_s = b'SSH-2.0-FortressOS_uSSH'

    cmd = [
        'ssh', '-v', '-p', str(port),
        '-o', 'StrictHostKeyChecking=no',
        '-o', 'UserKnownHostsFile=/dev/null',
        '-o', 'KexAlgorithms=curve25519-sha256',
        '-o', 'HostKeyAlgorithms=ssh-ed25519',
        '-o', 'Ciphers=chacha20-poly1305@openssh.com',
        '127.0.0.1'
    ]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        conn, _ = s.accept()
        conn.settimeout(5.0)

        client_id_line = b''
        while not client_id_line.endswith(b'\n'):
            client_id_line += conn.recv(1)
        V_c = client_id_line.rstrip(b'\r\n')
        conn.sendall(V_s + b'\r\n')

        def read_packet():
            hdr = conn.recv(4)
            while len(hdr) < 4:
                hdr += conn.recv(4 - len(hdr))
            pkt_len = struct.unpack('>I', hdr)[0]
            body = b''
            while len(body) < pkt_len:
                body += conn.recv(pkt_len - len(body))
            pad_len = body[0]
            return body[1:pkt_len - pad_len]

        def send_packet(payload):
            pad_len = 8 - ((len(payload) + 5) % 8)
            if pad_len < 4: pad_len += 8
            pkt_len = 1 + len(payload) + pad_len
            pkt = struct.pack('>IB', pkt_len, pad_len) + payload + (b'\x00' * pad_len)
            conn.sendall(pkt)

        I_c = read_packet()
        assert I_c[0] == 20

        def p_str(b): return struct.pack('>I', len(b)) + b
        I_s = (bytes([20]) + bytes([0x01]*16) +
               p_str(b'curve25519-sha256,kex-strict-s-v00@openssh.com') +
               p_str(b'ssh-ed25519') +
               p_str(b'chacha20-poly1305@openssh.com') +
               p_str(b'chacha20-poly1305@openssh.com') +
               p_str(b'') + p_str(b'') +
               p_str(b'none') + p_str(b'none') +
               p_str(b'') + p_str(b'') +
               b'\x00' + struct.pack('>I', 0))

        send_packet(I_s)

        ecdh_init = read_packet()
        assert ecdh_init[0] == 30
        q_c_len = struct.unpack('>I', ecdh_init[1:5])[0]
        Q_c = ecdh_init[5:5 + q_c_len]

        client_eph_pub = x25519.X25519PublicKey.from_public_bytes(Q_c)
        shared_secret_bytes = server_eph_priv.exchange(client_eph_pub)

        k_raw = shared_secret_bytes.lstrip(b'\x00')
        if len(k_raw) > 0 and (k_raw[0] & 0x80):
            k_mpint = struct.pack('>I', len(k_raw) + 1) + b'\x00' + k_raw
        else:
            k_mpint = struct.pack('>I', len(k_raw)) + k_raw

        def str_blob(b): return struct.pack('>I', len(b)) + b
        h_buf = (str_blob(V_c) + str_blob(V_s) + str_blob(I_c) + str_blob(I_s) +
                 str_blob(ks_blob) + str_blob(Q_c) + str_blob(server_eph_pub_bytes) +
                 k_mpint)
        H = hashlib.sha256(h_buf).digest()

        sig = host_priv.sign(H)
        sig_blob = p_str(b'ssh-ed25519') + p_str(sig)
        reply = bytes([31]) + p_str(ks_blob) + p_str(server_eph_pub_bytes) + p_str(sig_blob)
        send_packet(reply)
        send_packet(bytes([21]))

        client_newkeys = read_packet()
        assert client_newkeys[0] == 21, f"Expected NEWKEYS (21), got {client_newkeys[0]}"
        print("PASS: Live OpenSSH client accepted exchange hash H, verified Ed25519 host key, and sent SSH_MSG_NEWKEYS")
        conn.close()
    finally:
        s.close()
        p.kill()

if __name__ == '__main__':
    main()
