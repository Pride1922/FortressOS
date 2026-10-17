import socket, subprocess, struct, hashlib
from cryptography.hazmat.primitives.asymmetric import x25519, ed25519
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

# Deterministic private keys for reproducibility
host_priv = ed25519.Ed25519PrivateKey.from_private_bytes(bytes([0x42] * 32))
host_pub_bytes = host_priv.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
ks_blob = struct.pack('>I', 11) + b'ssh-ed25519' + struct.pack('>I', 32) + host_pub_bytes

server_eph_priv = x25519.X25519PrivateKey.from_private_bytes(bytes([0x77] * 32))
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
conn, _ = s.accept()

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
assert client_newkeys[0] == 21
conn.close()
s.close()
p.kill()

def format_c_array(name, data):
    lines = [f'static const uint8_t {name}[{len(data)}] = {{']
    for i in range(0, len(data), 12):
        chunk = data[i:i+12]
        lines.append('    ' + ', '.join(f'0x{b:02x}' for b in chunk) + ',')
    lines.append('};')
    return '\n'.join(lines)

code = f'''/* Auto-generated verified OpenSSH 9.6 handshake transcript ground-truth vector */
#ifndef FORTRESS_OPENSSH_GROUND_TRUTH_VECTOR_H
#define FORTRESS_OPENSSH_GROUND_TRUTH_VECTOR_H

#include <stdint.h>
#include <stddef.h>

static const char OPENSSH_GT_V_C[] = "{V_c.decode('latin1')}";
static const char OPENSSH_GT_V_S[] = "{V_s.decode('latin1')}";

{format_c_array('OPENSSH_GT_I_C', I_c)}

{format_c_array('OPENSSH_GT_I_S', I_s)}

{format_c_array('OPENSSH_GT_HOST_PUB', host_pub_bytes)}

{format_c_array('OPENSSH_GT_HOST_SEC', host_priv.private_bytes_raw() + host_pub_bytes)}

{format_c_array('OPENSSH_GT_Q_C', Q_c)}

{format_c_array('OPENSSH_GT_Q_S', server_eph_pub_bytes)}

{format_c_array('OPENSSH_GT_SHARED_SECRET_K', shared_secret_bytes)}

{format_c_array('OPENSSH_GT_HASH_H', H)}

#endif /* FORTRESS_OPENSSH_GROUND_TRUTH_VECTOR_H */
'''

with open('tests/openssh_ground_truth_vector.h', 'w') as f:
    f.write(code)

print('Generated tests/openssh_ground_truth_vector.h successfully!')
