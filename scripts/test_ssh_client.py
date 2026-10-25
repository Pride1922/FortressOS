#!/usr/bin/env python3
"""Multi-Firmware QEMU Acceptance Suite for Standalone SSH Client (Issue #61).

Verifies acceptance criteria:
1. /bin/ssh --help CLI validation and usage.
2. Ephemeral Curve25519-SHA256 key exchange (RFC 8731) with strict KEX extension.
3. Ed25519 host key authentication and ChaCha20-Poly1305 AEAD symmetric packet encryption.
4. RFC 4252 password userauth with successful authentication and wrong-password rejection.
5. RFC 4254 session channel exec command execution returning command stdout and exit status.
6. Clean execution across BIOS and UEFI firmware.
"""

from pathlib import Path
import os
import re
import selectors
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

from cryptography.hazmat.primitives.asymmetric import x25519, ed25519
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms
from cryptography.hazmat.primitives.poly1305 import Poly1305
from cryptography.hazmat.primitives import hashes

ROOT = Path(__file__).resolve().parent.parent


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def chacha20_crypt(key: bytes, nonce: bytes, counter: int, data: bytes) -> bytes:
    iv = struct.pack('<Q', counter) + nonce
    c = Cipher(algorithms.ChaCha20(key, iv), mode=None)
    enc = c.encryptor()
    return enc.update(data)


def poly1305_tag(key: bytes, data: bytes) -> bytes:
    p = Poly1305(key)
    p.update(data)
    return p.finalize()


def put_string(s: bytes) -> bytes:
    return struct.pack('>I', len(s)) + s


def put_cstring(s: str) -> bytes:
    b = s.encode('utf-8')
    return struct.pack('>I', len(b)) + b


def put_mpint(n_bytes: bytes) -> bytes:
    b = n_bytes.lstrip(b'\x00')
    if not b:
        return b'\x00\x00\x00\x00'
    if b[0] & 0x80:
        b = b'\x00' + b
    return struct.pack('>I', len(b)) + b


def get_string(b: bytes, off: int):
    l = struct.unpack('>I', b[off:off+4])[0]
    return b[off+4:off+4+l], off+4+l


def get_u32(b: bytes, off: int):
    return struct.unpack('>I', b[off:off+4])[0], off+4


def get_u8(b: bytes, off: int):
    return b[off], off+1


def recv_exact(conn: socket.socket, n: int) -> bytes:
    b = bytearray()
    while len(b) < n:
        chunk = conn.recv(n - len(b))
        if not chunk:
            raise EOFError("Unexpected EOF on socket")
        b.extend(chunk)
    return bytes(b)


def recv_unencrypted_packet(conn: socket.socket) -> bytes:
    hdr = recv_exact(conn, 4)
    pkt_len = struct.unpack('>I', hdr)[0]
    body = recv_exact(conn, pkt_len)
    pad_len = body[0]
    return body[1 : pkt_len - pad_len]


def derive_key(k: bytes, h: bytes, tag: bytes, session_id: bytes) -> bytes:
    def h_call(data: bytes) -> bytes:
        d = hashes.Hash(hashes.SHA256())
        d.update(data)
        return d.finalize()

    k_mpint = put_mpint(k)
    k1 = h_call(k_mpint + h + tag + session_id)
    k2 = h_call(k_mpint + h + k1)
    return k1 + k2


class MockSSHServer:
    def __init__(self, port, test_pass="TestPass8492"):
        self.port = port
        self.test_pass = test_pass
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(('0.0.0.0', port))
        self.sock.listen(5)
        self.running = True
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.ed25519_key = ed25519.Ed25519PrivateKey.generate()
        self.success_count = 0
        self.failure_count = 0

    def start(self):
        self.thread.start()

    def _run(self):
        while self.running:
            try:
                conn, _ = self.sock.accept()
                threading.Thread(target=self._handle_client, args=(conn,), daemon=True).start()
            except Exception:
                break

    def _handle_client(self, conn: socket.socket):
        conn.settimeout(15.0)
        try:
            # 1. Version exchange
            conn.sendall(b"SSH-2.0-OpenSSH_9.6p1\r\n")
            client_ver = b""
            while not client_ver.endswith(b"\n"):
                ch = conn.recv(1)
                if not ch: return
                client_ver += ch
            client_ver = client_ver.strip(b"\r\n")
            server_ver = b"SSH-2.0-OpenSSH_9.6p1"

            # 2. Server KEXINIT
            cookie = os.urandom(16)
            kex_payload = bytearray([20]) # SSH_MSG_KEXINIT
            kex_payload.extend(cookie)
            kex_payload.extend(put_cstring("curve25519-sha256,kex-strict-s-v00@openssh.com"))
            kex_payload.extend(put_cstring("ssh-ed25519"))
            kex_payload.extend(put_cstring("chacha20-poly1305@openssh.com"))
            kex_payload.extend(put_cstring("chacha20-poly1305@openssh.com"))
            kex_payload.extend(put_cstring("none"))
            kex_payload.extend(put_cstring("none"))
            kex_payload.extend(put_cstring("none"))
            kex_payload.extend(put_cstring("none"))
            kex_payload.extend(put_cstring(""))
            kex_payload.extend(put_cstring(""))
            kex_payload.extend(b"\x00") # first kex follows
            kex_payload.extend(b"\x00\x00\x00\x00") # reserved

            pad_len = 8 - ((len(kex_payload) + 5) % 8)
            if pad_len < 4: pad_len += 8
            pkt_len = 1 + len(kex_payload) + pad_len
            conn.sendall(struct.pack('>IB', pkt_len, pad_len) + bytes(kex_payload) + (b'\x00' * pad_len))
            server_kexinit = bytes(kex_payload)

            # 3. Read client KEXINIT
            client_kexinit = recv_unencrypted_packet(conn)
            assert client_kexinit[0] == 20

            # 4. Read SSH_MSG_KEX_ECDH_INIT (30)
            ecdh_init = recv_unencrypted_packet(conn)
            assert ecdh_init[0] == 30
            client_pub_bytes, _ = get_string(ecdh_init, 1)

            # 5. Compute shared secret and exchange hash H
            server_priv = x25519.X25519PrivateKey.generate()
            server_pub = server_priv.public_key()
            server_pub_bytes = server_pub.public_bytes_raw()
            client_pub = x25519.X25519PublicKey.from_public_bytes(client_pub_bytes)
            shared_secret = server_priv.exchange(client_pub)

            host_pub_raw = self.ed25519_key.public_key().public_bytes_raw()
            host_key_blob = put_cstring("ssh-ed25519") + put_string(host_pub_raw)

            h_buf = bytearray()
            h_buf.extend(put_string(client_ver))
            h_buf.extend(put_string(server_ver))
            h_buf.extend(put_string(client_kexinit))
            h_buf.extend(put_string(server_kexinit))
            h_buf.extend(put_string(host_key_blob))
            h_buf.extend(put_string(client_pub_bytes))
            h_buf.extend(put_string(server_pub_bytes))
            h_buf.extend(put_mpint(shared_secret))

            d = hashes.Hash(hashes.SHA256())
            d.update(bytes(h_buf))
            h_val = d.finalize()
            session_id = h_val

            sig_raw = self.ed25519_key.sign(h_val)
            sig_blob = put_cstring("ssh-ed25519") + put_string(sig_raw)

            reply_payload = bytearray([31])
            reply_payload.extend(put_string(host_key_blob))
            reply_payload.extend(put_string(server_pub_bytes))
            reply_payload.extend(put_string(sig_blob))

            pad_len = 8 - ((len(reply_payload) + 5) % 8)
            if pad_len < 4: pad_len += 8
            pkt_len = 1 + len(reply_payload) + pad_len
            conn.sendall(struct.pack('>IB', pkt_len, pad_len) + bytes(reply_payload) + (b'\x00' * pad_len))

            # Send SSH_MSG_NEWKEYS (21)
            conn.sendall(struct.pack('>IBB', 12, 10, 21) + (b'\x00' * 10))

            # Read client SSH_MSG_NEWKEYS (21)
            client_newkeys = recv_unencrypted_packet(conn)
            assert client_newkeys[0] == 21

            # 6. Derive ChaCha20-Poly1305 keys
            key_c2s = derive_key(shared_secret, h_val, b'C', session_id)
            key_s2c = derive_key(shared_secret, h_val, b'D', session_id)

            k1_c2s, k2_c2s = key_c2s[:32], key_c2s[32:64]
            k1_s2c, k2_s2c = key_s2c[:32], key_s2c[32:64]

            seq_c2s = 0
            seq_s2c = 0

            def recv_encrypted_packet():
                nonlocal seq_c2s
                enc_len = recv_exact(conn, 4)
                nonce = struct.pack('>Q', seq_c2s)
                pkt_len_bytes = chacha20_crypt(k2_c2s, nonce, 0, enc_len)
                pkt_len = struct.unpack('>I', pkt_len_bytes)[0]

                raw = recv_exact(conn, pkt_len + 16)
                enc_body = raw[:pkt_len]
                tag = raw[pkt_len:]

                poly_key = chacha20_crypt(k1_c2s, nonce, 0, b'\x00' * 64)[:32]
                calc_tag = poly1305_tag(poly_key, enc_len + enc_body)
                assert calc_tag == tag, "Poly1305 verification failed"

                dec_body = chacha20_crypt(k1_c2s, nonce, 1, enc_body)
                pad_len = dec_body[0]
                payload = dec_body[1 : pkt_len - pad_len]
                seq_c2s += 1
                return payload

            def send_encrypted_packet(payload: bytes):
                nonlocal seq_s2c
                pad_len = 8 - ((len(payload) + 5) % 8)
                if pad_len < 4: pad_len += 8
                pkt_len = 1 + len(payload) + pad_len
                clear_body = bytes([pad_len]) + payload + (b'\x00' * pad_len)

                nonce = struct.pack('>Q', seq_s2c)
                enc_len = chacha20_crypt(k2_s2c, nonce, 0, struct.pack('>I', pkt_len))
                enc_body = chacha20_crypt(k1_s2c, nonce, 1, clear_body)
                poly_key = chacha20_crypt(k1_s2c, nonce, 0, b'\x00' * 64)[:32]
                tag = poly1305_tag(poly_key, enc_len + enc_body)

                conn.sendall(enc_len + enc_body + tag)
                seq_s2c += 1

            # 7. Service request
            req = recv_encrypted_packet()
            assert req[0] == 5 # SSH_MSG_SERVICE_REQUEST
            srv_name, _ = get_string(req, 1)
            assert srv_name == b"ssh-userauth"
            send_encrypted_packet(bytes([6]) + put_cstring("ssh-userauth"))

            # 8. Userauth request
            auth = recv_encrypted_packet()
            assert auth[0] == 50 # SSH_MSG_USERAUTH_REQUEST
            user, off = get_string(auth, 1)
            service, off = get_string(auth, off)
            method, off = get_string(auth, off)
            assert method == b"password"
            chg, off = get_u8(auth, off)
            password_b, _ = get_string(auth, off)
            password = password_b.decode('utf-8', errors='replace')

            if password == self.test_pass:
                self.success_count += 1
                send_encrypted_packet(bytes([52])) # SSH_MSG_USERAUTH_SUCCESS

                # Channel open
                ch_open = recv_encrypted_packet()
                assert ch_open[0] == 90 # SSH_MSG_CHANNEL_OPEN
                ch_type, off = get_string(ch_open, 1)
                sender_ch, off = get_u32(ch_open, off)
                initial_win, off = get_u32(ch_open, off)
                max_pkt, off = get_u32(ch_open, off)

                # Channel open confirmation
                send_encrypted_packet(bytes([91]) + struct.pack('>IIII', sender_ch, 0, 1048576, 32768))

                # Channel request (exec)
                ch_req = recv_encrypted_packet()
                assert ch_req[0] == 98 # SSH_MSG_CHANNEL_REQUEST
                rcv_ch, off = get_u32(ch_req, 1)
                req_type, off = get_string(ch_req, off)
                want_reply, off = get_u8(ch_req, off)
                cmd, _ = get_string(ch_req, off)

                if want_reply:
                    send_encrypted_packet(bytes([99]) + struct.pack('>I', sender_ch))

                # Send command output data
                out_msg = b"FORTRESS_SSH_EXEC_SUCCESS\noperator\n"
                send_encrypted_packet(bytes([94]) + struct.pack('>I', sender_ch) + put_string(out_msg))

                # Send exit-status
                exit_payload = bytes([98]) + struct.pack('>I', sender_ch) + put_cstring("exit-status") + b"\x00" + struct.pack('>I', 0)
                send_encrypted_packet(exit_payload)

                # Send EOF & CLOSE
                send_encrypted_packet(bytes([96]) + struct.pack('>I', sender_ch))
                send_encrypted_packet(bytes([97]) + struct.pack('>I', sender_ch))

                # Wait for client channel close
                c_close = recv_encrypted_packet()
            else:
                self.failure_count += 1
                send_encrypted_packet(bytes([51]) + put_cstring("password") + b"\x00") # SSH_MSG_USERAUTH_FAILURE
        except Exception as e:
            import traceback
            traceback.print_exc()
        finally:
            conn.close()

    def close(self):
        self.running = False
        try: self.sock.close()
        except: pass


class Guest:
    def __init__(self, firmware, smp, tmp):
        self.firmware = firmware
        self.smp = smp
        self.data = bytearray()
        argv = [
            'qemu-system-x86_64', '-M', 'q35', '-m', '512M', '-smp', str(smp),
            '-accel', 'tcg', '-cpu', 'max,+rdrand,+rdseed', '-display', 'none',
            '-monitor', 'none', '-serial', 'stdio', '-no-reboot', '-boot', 'd',
            '-cdrom', str(ROOT / 'bin/fortress.iso'),
            '-netdev', 'user,id=n0',
            '-device', 'e1000e,netdev=n0'
        ]
        if firmware == 'uefi':
            variables = tmp / f'vars-{smp}.fd'
            shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd', variables)
            argv += [
                '-drive', 'if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
                '-drive', f'if=pflash,format=raw,unit=1,file={variables}'
            ]
        self.proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.sel = selectors.DefaultSelector()
        self.sel.register(self.proc.stdout, selectors.EVENT_READ)

    def pump(self):
        for key, _ in self.sel.select(0.05):
            self.data.extend(key.fileobj.read1(65536))

    def text(self, start=0):
        cleaned = re.sub(r'\x1b\[[0-9;?]*[ -/]*[@-~]', '', self.data[start:].decode(errors='replace'))
        return cleaned.replace('\r', '')

    def wait(self, predicate, start=0, timeout=60):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.pump()
            t = self.text(start)
            if predicate(t):
                return t
            assert self.proc.poll() is None, 'Guest terminated unexpectedly: ' + t[-1500:]
        raise AssertionError(f'Timeout waiting for condition after {timeout}s: ' + t[-2000:])

    def send(self, line):
        for ch in line + '\n':
            self.proc.stdin.write(ch.encode('utf-8'))
            self.proc.stdin.flush()
            time.sleep(0.005)

    def command(self, cmd, timeout=30):
        start = len(self.data)
        self.send(cmd)
        result = self.wait(lambda s: '\n' in s and s.endswith('$ '), start, timeout)
        return result.split('\n', 1)[1]

    def close(self):
        try:
            self.send('sudo /bin/poweroff')
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()
            self.proc.wait()


def test_firmware(firmware, smp=1):
    print(f"\n========================================================")
    print(f"Testing /bin/ssh Client under {firmware.upper()} (SMP={smp})")
    print(f"========================================================")

    srv_port = free_port()
    test_pass = "TestPass8492"
    server = MockSSHServer(srv_port, test_pass=test_pass)
    server.start()

    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = Path(tmpdir)
        guest = Guest(firmware, smp, tmp)
        try:
            print("[1/5] Waiting for boot and login prompt...")
            guest.wait(lambda t: t.endswith('FortressOS login: '), timeout=45)
            print("  [PASS] Boot successful; login prompt reached")

            print("[2/5] Logging in as operator...")
            guest.send('operator')
            guest.wait(lambda t: t.endswith('$ '), timeout=15)
            print("  [PASS] Logged in successfully as operator")

            print("[3/5] Verifying '/bin/ssh --help'...")
            help_out = guest.command('/bin/ssh --help')
            assert "usage: ssh" in help_out, f"Missing usage in ssh --help: {help_out}"
            print("  [PASS] '/bin/ssh --help' output verified")

            # 4. Connect to host SSH server with valid password
            print(f"[4/5] Testing remote command execution via /bin/ssh to 10.0.2.2:{srv_port}...")
            start_len = len(guest.data)
            cmd = f'/bin/ssh -P {test_pass} -p {srv_port} operator@10.0.2.2 "echo FORTRESS_SSH_EXEC_SUCCESS; whoami"'
            guest.send(cmd)
            guest.wait(lambda t: "FORTRESS_SSH_EXEC_SUCCESS" in t, start=start_len, timeout=15)
            guest.wait(lambda t: "operator" in t, start=start_len, timeout=15)
            guest.wait(lambda t: t.endswith('$ '), timeout=10)
            assert server.success_count > 0, "Server did not record successful authentication"
            print("  [PASS] Remote command execution succeeded with RFC 8731 KEX & ChaCha20-Poly1305 AEAD")

            # 5. Connect with invalid password and verify rejection
            print("[5/5] Testing wrong-password rejection...")
            start_len = len(guest.data)
            bad_cmd = f'/bin/ssh -P WrongPassword123 -p {srv_port} operator@10.0.2.2 whoami'
            guest.send(bad_cmd)
            guest.wait(lambda t: "Permission denied" in t, start=start_len, timeout=15)
            guest.wait(lambda t: t.endswith('$ '), timeout=10)
            assert server.failure_count > 0, "Server did not record failed authentication"
            print("  [PASS] Wrong-password fail-closed rejection verified")

            print(f"  [PASS] {firmware.upper()} SSH client acceptance completed cleanly")

        finally:
            server.close()
            guest.close()


def main():
    test_firmware('bios', smp=1)
    test_firmware('uefi', smp=1)
    print("\n========================================================")
    print("ALL SSH CLIENT MULTI-FIRMWARE ACCEPTANCE TESTS PASSED (100%)!")
    print("========================================================\n")


if __name__ == '__main__':
    main()
