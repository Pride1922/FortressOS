#!/usr/bin/env python3
"""
scripts/test_tls_fixture.py
Phase TLS-3: Scripted TCP Proxy & Deterministic Adversarial Gates runner and fixture.
Orchestrates and verifies the 16 deterministic adversarial test cases specified in docs/plans/TLS_PLAN.md §6.3.
"""

import os
import sys
import time
import socket
import ssl
import struct
import shutil
import tempfile
import threading
import subprocess
import datetime
from cryptography import x509
from cryptography.x509.oid import NameOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa, padding

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BUILD_DIR = os.path.join(REPO_ROOT, "build", "tls_host")
BIN_OUT = os.path.join(BUILD_DIR, "tls_fixture_host")
LIB_TLS = os.path.join(BUILD_DIR, "libfortresstls.a")

def generate_pki_hierarchy(workdir):
    """Generates all test certificates and compiles test_anchors.v1."""
    certs = {}
    now = datetime.datetime.now(datetime.timezone.utc)

    # 1. Root CA (Fortress Test Root CA)
    ca_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    ca_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "Fortress Test Root CA")])
    ca_cert = (x509.CertificateBuilder()
        .subject_name(ca_name)
        .issuer_name(ca_name)
        .public_key(ca_key.public_key())
        .serial_number(1)
        .not_valid_before(now - datetime.timedelta(days=10))
        .not_valid_after(now + datetime.timedelta(days=730))
        .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
        .sign(ca_key, hashes.SHA256()))

    ca_pem_path = os.path.join(workdir, "test_ca.pem")
    ca_key_path = os.path.join(workdir, "test_ca.key")
    with open(ca_pem_path, "wb") as f:
        f.write(ca_cert.public_bytes(serialization.Encoding.PEM))
    with open(ca_key_path, "wb") as f:
        f.write(ca_key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))

    certs["ca_pem"] = ca_pem_path
    certs["ca_key"] = ca_key_path

    # Compile test_anchors.v1 using build_trust_anchors.py
    anchors_bin_path = os.path.join(workdir, "test_anchors.v1")
    compile_cmd = [
        sys.executable,
        os.path.join(REPO_ROOT, "scripts", "build_trust_anchors.py"),
        "--input", ca_pem_path,
        "--out-bin", anchors_bin_path
    ]
    res = subprocess.run(compile_cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"FAIL: Compiling test_anchors.v1 failed:\n{res.stderr}")
        sys.exit(1)
    certs["anchors_v1"] = anchors_bin_path

    def make_server_cert(prefix, key_size, cn, sans, not_before, not_after,
                         sign_key=ca_key, sign_name=ca_name, pss=False, self_signed=False):
        s_key = rsa.generate_private_key(public_exponent=65537, key_size=key_size)
        s_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)])
        builder = (x509.CertificateBuilder()
            .subject_name(s_name)
            .issuer_name(s_name if self_signed else sign_name)
            .public_key(s_key.public_key())
            .serial_number(int(time.time() * 1000) % 10000000)
            .not_valid_before(not_before)
            .not_valid_after(not_after))
        if sans:
            builder = builder.add_extension(
                x509.SubjectAlternativeName([x509.DNSName(s) for s in sans]),
                critical=False
            )
        effective_key = s_key if self_signed else sign_key
        if pss:
            sig_pad = padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=padding.PSS.MAX_LENGTH)
        else:
            sig_pad = padding.PKCS1v15()
        s_cert = builder.sign(effective_key, hashes.SHA256(), sig_pad)

        crt_path = os.path.join(workdir, f"{prefix}.crt")
        key_path = os.path.join(workdir, f"{prefix}.key")
        with open(crt_path, "wb") as f:
            f.write(s_cert.public_bytes(serialization.Encoding.PEM))
            if not self_signed:
                # Include intermediate / CA bundle in server certificate chain
                f.write(ca_cert.public_bytes(serialization.Encoding.PEM))
        with open(key_path, "wb") as f:
            f.write(s_key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
        certs[f"{prefix}_crt"] = crt_path
        certs[f"{prefix}_key"] = key_path

    # Valid: 2026-01-01 to 2027-01-01, SAN ["test.fortress.local", "localhost", "127.0.0.1"]
    make_server_cert("valid", 2048, "test.fortress.local",
                     ["test.fortress.local", "localhost"],
                     now - datetime.timedelta(days=1), now + datetime.timedelta(days=365))

    # Expired: 2024-01-01 to 2025-01-01
    make_server_cert("expired", 2048, "test.fortress.local",
                     ["test.fortress.local"],
                     datetime.datetime(2024, 1, 1, tzinfo=datetime.timezone.utc),
                     datetime.datetime(2025, 1, 1, tzinfo=datetime.timezone.utc))

    # Not-yet-valid: 2028-01-01 to 2029-01-01
    make_server_cert("not_yet_valid", 2048, "test.fortress.local",
                     ["test.fortress.local"],
                     datetime.datetime(2028, 1, 1, tzinfo=datetime.timezone.utc),
                     datetime.datetime(2029, 1, 1, tzinfo=datetime.timezone.utc))

    # Self-signed (untrusted)
    make_server_cert("self_signed", 2048, "test.fortress.local",
                     ["test.fortress.local"],
                     now - datetime.timedelta(days=1), now + datetime.timedelta(days=365),
                     self_signed=True)

    # Wrong SAN: SAN ["wrong.fortress.local"]
    make_server_cert("wrong_san", 2048, "wrong.fortress.local",
                     ["wrong.fortress.local"],
                     now - datetime.timedelta(days=1), now + datetime.timedelta(days=365))

    # Sub-2048-bit RSA key: 1024 bits
    make_server_cert("sub_2048", 1024, "test.fortress.local",
                     ["test.fortress.local"],
                     now - datetime.timedelta(days=1), now + datetime.timedelta(days=365))

    # RSA-PSS signature
    make_server_cert("rsa_pss", 2048, "test.fortress.local",
                     ["test.fortress.local"],
                     now - datetime.timedelta(days=1), now + datetime.timedelta(days=365),
                     pss=True)

    return certs

def read_tls_record(sock):
    """Reads a full TLS record from socket."""
    hdr = b""
    while len(hdr) < 5:
        chunk = sock.recv(5 - len(hdr))
        if not chunk:
            return None
        hdr += chunk
    ctype, ver, length = struct.unpack(">BHH", hdr)
    body = b""
    while len(body) < length:
        chunk = sock.recv(length - len(body))
        if not chunk:
            break
        body += chunk
    return hdr + body

def run_fixture_server(case_name, certs, ready_event, stop_event, port_holder):
    """Runs the scripted TCP proxy / server fixture for a specific test case."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    port_holder[0] = srv.getsockname()[1]
    ready_event.set()

    try:
        srv.settimeout(5.0)
        conn, _ = srv.accept()
    except socket.timeout:
        srv.close()
        return

    try:
        conn.settimeout(5.0)

        if case_name == "case1_valid":
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(certs["valid_crt"], certs["valid_key"])
            s_conn = ctx.wrap_socket(conn, server_side=True)
            req = s_conn.recv(1024)
            s_conn.sendall(b"HTTP/1.0 200 OK\r\nContent-Length: 5\r\n\r\nHELLO")
            time.sleep(0.05)
            s_conn.unwrap()
            s_conn.close()

        elif case_name in ("case2_expired", "case3_not_yet_valid", "case4_self_signed",
                           "case5_wrong_hostname", "case6_sub_2048_rsa"):
            prefix = {
                "case2_expired": "expired",
                "case3_not_yet_valid": "not_yet_valid",
                "case4_self_signed": "self_signed",
                "case5_wrong_hostname": "wrong_san",
                "case6_sub_2048_rsa": "sub_2048"
            }[case_name]
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            if prefix == "sub_2048":
                ctx.set_ciphers("DEFAULT@SECLEVEL=0")
            ctx.load_cert_chain(certs[f"{prefix}_crt"], certs[f"{prefix}_key"])
            try:
                s_conn = ctx.wrap_socket(conn, server_side=True)
                s_conn.close()
            except (ssl.SSLError, ConnectionResetError, BrokenPipeError):
                pass

        elif case_name == "case7_rsa_pss":
            # Proxy connection to local loopback SSL server, intercepting Certificate record
            # to replace sha256WithRSAEncryption OID with id-RSASSA-PSS (1.2.840.113549.1.1.10)
            backend_srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            backend_srv.bind(("127.0.0.1", 0))
            backend_srv.listen(1)
            b_port = backend_srv.getsockname()[1]

            def backend_worker():
                b_conn, _ = backend_srv.accept()
                b_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                b_ctx.load_cert_chain(certs["valid_crt"], certs["valid_key"])
                try:
                    b_ssl = b_ctx.wrap_socket(b_conn, server_side=True)
                    time.sleep(0.1)
                    b_ssl.close()
                except Exception:
                    pass
                finally:
                    backend_srv.close()

            bt = threading.Thread(target=backend_worker)
            bt.daemon = True
            bt.start()

            proxy_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            proxy_sock.connect(("127.0.0.1", b_port))

            # 1. Forward ClientHello (client -> backend)
            rec = read_tls_record(conn)
            proxy_sock.sendall(rec)

            # 2. Forward ServerHello ... ServerHelloDone (backend -> client)
            # Replace sha256WithRSA (06 09 2A 86 48 86 F7 0D 01 01 0B)
            # with id-RSASSA-PSS (06 09 2A 86 48 86 F7 0D 01 01 0A) in Certificate record
            target_oid = b"\x2a\x86\x48\x86\xf7\x0d\x01\x01\x0b"
            pss_oid    = b"\x2a\x86\x48\x86\xf7\x0d\x01\x01\x0a"
            while True:
                rec = read_tls_record(proxy_sock)
                if not rec:
                    break
                if rec[0] == 22 and target_oid in rec:
                    rec = rec.replace(target_oid, pss_oid)
                conn.sendall(rec)
                if rec[0] == 22 and (len(rec) >= 9 and rec[5] == 14): # ServerHelloDone
                    break

            time.sleep(0.1)
            proxy_sock.close()
            conn.close()

        elif case_name == "case8_unsupported_cipher":
            # Read ClientHello
            _ = read_tls_record(conn)
            # Send ServerHello with static RSA / CBC cipher 0x002F (TLS_RSA_WITH_AES_128_CBC_SHA)
            # Record header (5B) + Handshake (44B)
            raw_server_hello = (
                b"\x16\x03\x03\x00\x2c"  # Handshake record, TLS 1.2, length 44
                b"\x02\x00\x00\x28"      # ServerHello, length 40
                b"\x03\x03"              # Version TLS 1.2
                + (b"\x5a" * 32)         # Server Random
                + b"\x00"                # Session ID length 0
                + b"\x00\x2f"            # Cipher Suite: TLS_RSA_WITH_AES_128_CBC_SHA
                + b"\x00"                # Compression: None
                + b"\x00\x00"            # Extensions length: 0
            )
            conn.sendall(raw_server_hello)
            time.sleep(0.1)
            conn.close()

        elif case_name == "case9_corrupted_tag":
            # Proxy connection to local loopback SSL server
            backend_srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            backend_srv.bind(("127.0.0.1", 0))
            backend_srv.listen(1)
            b_port = backend_srv.getsockname()[1]

            def backend_worker():
                b_conn, _ = backend_srv.accept()
                b_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                b_ctx.load_cert_chain(certs["valid_crt"], certs["valid_key"])
                try:
                    b_ssl = b_ctx.wrap_socket(b_conn, server_side=True)
                    _ = b_ssl.recv(1024)
                    b_ssl.sendall(b"HTTP/1.0 200 OK\r\nContent-Length: 5\r\n\r\nHELLO")
                    time.sleep(0.1)
                    b_ssl.close()
                except Exception:
                    pass
                finally:
                    backend_srv.close()

            bt = threading.Thread(target=backend_worker)
            bt.daemon = True
            bt.start()

            proxy_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            proxy_sock.connect(("127.0.0.1", b_port))

            # 1. Forward ClientHello (client -> backend)
            rec = read_tls_record(conn)
            proxy_sock.sendall(rec)

            # 2. Forward ServerHello ... ServerHelloDone (backend -> client)
            while True:
                rec = read_tls_record(proxy_sock)
                conn.sendall(rec)
                if rec[0] == 22 and (len(rec) >= 9 and rec[5] == 14): # ServerHelloDone
                    break

            # 3. Forward ClientKeyExchange, ChangeCipherSpec, Finished (client -> backend)
            for _ in range(3):
                rec = read_tls_record(conn)
                proxy_sock.sendall(rec)

            # 4. Forward ChangeCipherSpec, Finished (backend -> client)
            for _ in range(2):
                rec = read_tls_record(proxy_sock)
                conn.sendall(rec)

            # Handshake complete!
            # 5. Forward Application Data HTTP Request (client -> backend)
            rec = read_tls_record(conn)
            proxy_sock.sendall(rec)

            # 6. Receive Application Data HTTP Response (backend -> client), corrupt AEAD tag
            rec = read_tls_record(proxy_sock)
            if rec and rec[0] == 23:
                corrupted = bytearray(rec)
                corrupted[-1] ^= 0xFF # Corrupt last byte (AEAD auth tag)
                conn.sendall(corrupted)

            time.sleep(0.1)
            proxy_sock.close()
            conn.close()

        elif case_name == "case10_malformed_chunked":
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(certs["valid_crt"], certs["valid_key"])
            s_conn = ctx.wrap_socket(conn, server_side=True)
            _ = s_conn.recv(1024)
            # Send HTTP response with invalid chunk header (non-hex characters ZZZZ)
            s_conn.sendall(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZZZZ\r\nBadData\r\n0\r\n\r\n")
            time.sleep(0.05)
            s_conn.unwrap()
            s_conn.close()

        elif case_name == "case11_slow_loris":
            # Read ClientHello
            _ = read_tls_record(conn)
            # Trickle ServerHello byte-by-byte (1 byte every 300ms)
            raw_server_hello = (
                b"\x16\x03\x03\x00\x2c"
                b"\x02\x00\x00\x28"
                b"\x03\x03"
                + (b"\x5a" * 32)
                + b"\x00\x00\x2f\x00\x00\x00"
            )
            for b in raw_server_hello:
                try:
                    conn.send(bytes([b]))
                    time.sleep(0.3)
                except (socket.error, BrokenPipeError):
                    break
            conn.close()

        elif case_name == "case12_abrupt_drop":
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(certs["valid_crt"], certs["valid_key"])
            s_conn = ctx.wrap_socket(conn, server_side=True)
            _ = s_conn.recv(1024)
            s_conn.sendall(b"HTTP/1.0 200 OK\r\nContent-Length: 5\r\n\r\nHELLO")
            time.sleep(0.05)
            # Drop TCP connection immediately WITHOUT sending close_notify alert
            s_conn.close()

        elif case_name == "case13_content_length_truncation":
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(certs["valid_crt"], certs["valid_key"])
            s_conn = ctx.wrap_socket(conn, server_side=True)
            _ = s_conn.recv(1024)
            # Send Content-Length: 100 with only 14 bytes body
            s_conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nOnly 14 bytes!")
            time.sleep(0.05)
            s_conn.unwrap() # Sends clean close_notify alert
            s_conn.close()

    except (ssl.SSLError, ConnectionResetError, BrokenPipeError):
        pass
    except Exception as e:
        import traceback
        traceback.print_exc()
    finally:
        srv.close()

def main():
    print("======================================================================")
    print("  FortressOS Phase TLS-3: Scripted TCP Proxy & Adversarial Gates      ")
    print("======================================================================")

    # 1. Compile host test binary with AddressSanitizer and UndefinedBehaviorSanitizer
    print("=== [1/4] Building tls_fixture_host under ASan/UBSan ===")
    test_srcs = [
        os.path.join(REPO_ROOT, "tests", "tls_fixture_host.c"),
        os.path.join(REPO_ROOT, "user", "wget_codec.c"),
        os.path.join(REPO_ROOT, "src", "lib", "tls", "anchor_parser.c"),
    ]
    compile_cmd = [
        "gcc",
        "-std=c99",
        "-O1",
        "-g",
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-DTEST_HOST=1",
        "-I" + REPO_ROOT,
        "-I" + os.path.join(REPO_ROOT, "src", "include"),
        "-I" + os.path.join(REPO_ROOT, "src", "lib", "tls"),
        "-I" + os.path.join(REPO_ROOT, "src", "lib", "tls", "bearssl", "inc"),
        "-I" + os.path.join(REPO_ROOT, "src", "lib", "tls", "bearssl", "src"),
        "-I" + os.path.join(REPO_ROOT, "user"),
        "-I" + os.path.join(REPO_ROOT, "tests"),
    ] + test_srcs + [LIB_TLS, "-o", BIN_OUT]

    res = subprocess.run(compile_cmd)
    if res.returncode != 0:
        print("FAIL: Compilation of tls_fixture_host failed")
        sys.exit(1)
    print("    -> Successfully built:", BIN_OUT)

    # 2. Generate PKI test hierarchy
    print("=== [2/4] Generating PKI test hierarchy & trust anchor table ===")
    workdir = tempfile.mkdtemp(prefix="fortress_tls_test_")
    try:
        certs = generate_pki_hierarchy(workdir)
        print("    -> PKI test suite ready in:", workdir)

        # 3. Execute 16 Deterministic Adversarial Gates
        print("=== [3/4] Executing 16 Deterministic Adversarial Test Cases ===")
        all_cases = [
            ("case1_valid", "Valid certificate handshake", True),
            ("case2_expired", "Expired certificate rejection", True),
            ("case3_not_yet_valid", "Not-yet-valid certificate rejection", True),
            ("case4_self_signed", "Self-signed certificate rejection", True),
            ("case5_wrong_hostname", "Wrong hostname / SAN mismatch rejection", True),
            ("case6_sub_2048_rsa", "Sub-2048-bit RSA key rejection", True),
            ("case7_rsa_pss", "RSA-PSS signature rejection", True),
            ("case8_unsupported_cipher", "Unsupported cipher suite negotiation rejection", True),
            ("case9_corrupted_tag", "Corrupted record authentication tag rejection", True),
            ("case10_malformed_chunked", "Malformed chunked transfer encoding framing rejection", True),
            ("case11_slow_loris", "Slow-loris 1-byte fragmented handshake timeout", True),
            ("case12_abrupt_drop", "Abrupt TCP drop without close_notify rejection", True),
            ("case13_content_length_truncation", "Content-Length truncation rejection", True),
            ("case14_entropy_failure", "Entropy failure / short read injection", False),
            ("case15_invalid_clock", "Invalid / uncalibrated clock rejection", False),
            ("case16_missing_malformed_anchors", "Missing / malformed anchors.v1 fail-closed", False),
        ]

        passed = 0
        for case_id, desc, is_network in all_cases:
            if is_network:
                ready_event = threading.Event()
                stop_event = threading.Event()
                port_holder = [0]
                t = threading.Thread(target=run_fixture_server,
                                     args=(case_id, certs, ready_event, stop_event, port_holder))
                t.daemon = True
                t.start()
                ready_event.wait(timeout=2.0)
                port = port_holder[0]

                cmd = [BIN_OUT, case_id, str(port), certs["anchors_v1"]]
                res = subprocess.run(cmd)
                t.join(timeout=2.0)
                if res.returncode != 0:
                    print(f"FAIL: {case_id} ({desc}) failed with exit code {res.returncode}")
                    sys.exit(1)
            else:
                cmd = [BIN_OUT, case_id, "0", certs["anchors_v1"]]
                res = subprocess.run(cmd)
                if res.returncode != 0:
                    print(f"FAIL: {case_id} ({desc}) failed with exit code {res.returncode}")
                    sys.exit(1)

            passed += 1

        print(f"=== [4/4] Verification Summary: {passed}/16 adversarial test cases PASSED ===")
        print(">>> 100% PASS: All Phase TLS-3 Adversarial Gates Validated Under ASan/UBSan <<<")

    finally:
        shutil.rmtree(workdir, ignore_errors=True)

if __name__ == "__main__":
    main()
