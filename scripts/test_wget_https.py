#!/usr/bin/env python3
"""
scripts/test_wget_https.py
Phase TLS-4: HTTPS Client Extension in /bin/wget verification suite under ASan/UBSan.
Tests end-to-end HTTPS downloading, chunked decoding, downgrade refusal,
strict HTTP 200, mandatory authenticated close_notify teardown, and atomic rename.
"""

import os
import sys
import time
import socket
import ssl
import shutil
import tempfile
import threading
import subprocess
import datetime
import ipaddress
from cryptography import x509
from cryptography.x509.oid import NameOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BUILD_DIR = os.path.join(REPO_ROOT, "build", "tls_host")
BIN_OUT = os.path.join(BUILD_DIR, "wget_https_host")
LIB_TLS = os.path.join(BUILD_DIR, "libfortresstls.a")

def generate_pki(workdir):
    """Generates test root CA and server certificates for test.fortress.local."""
    now = datetime.datetime.now(datetime.timezone.utc)

    # 1. Root CA
    ca_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    ca_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "Fortress Test Wget Root CA")])
    ca_cert = (x509.CertificateBuilder()
        .subject_name(ca_name)
        .issuer_name(ca_name)
        .public_key(ca_key.public_key())
        .serial_number(1)
        .not_valid_before(now - datetime.timedelta(days=10))
        .not_valid_after(now + datetime.timedelta(days=730))
        .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
        .sign(ca_key, hashes.SHA256()))

    ca_pem_path = os.path.join(workdir, "ca.pem")
    with open(ca_pem_path, "wb") as f:
        f.write(ca_cert.public_bytes(serialization.Encoding.PEM))

    # Compile test_anchors.v1
    anchors_bin_path = os.path.join(workdir, "anchors.v1")
    compile_cmd = [
        sys.executable,
        os.path.join(REPO_ROOT, "scripts", "build_trust_anchors.py"),
        "--input", ca_pem_path,
        "--out-bin", anchors_bin_path
    ]
    res = subprocess.run(compile_cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"FAIL: Compiling test anchors.v1 failed:\n{res.stderr}")
        sys.exit(1)

    # 2. Server Certificate for test.fortress.local and 127.0.0.1
    srv_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    srv_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "test.fortress.local")])
    srv_cert = (x509.CertificateBuilder()
        .subject_name(srv_name)
        .issuer_name(ca_name)
        .public_key(srv_key.public_key())
        .serial_number(2)
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=365))
        .add_extension(
            x509.SubjectAlternativeName([
                x509.DNSName("test.fortress.local"),
                x509.IPAddress(ipaddress.IPv4Address("127.0.0.1"))
            ]),
            critical=False
        )
        .sign(ca_key, hashes.SHA256()))

    srv_crt_path = os.path.join(workdir, "server.crt")
    srv_key_path = os.path.join(workdir, "server.key")
    with open(srv_crt_path, "wb") as f:
        f.write(srv_cert.public_bytes(serialization.Encoding.PEM))
        f.write(ca_cert.public_bytes(serialization.Encoding.PEM))
    with open(srv_key_path, "wb") as f:
        f.write(srv_key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))

    return {
        "ca_pem": ca_pem_path,
        "anchors_v1": anchors_bin_path,
        "server_crt": srv_crt_path,
        "server_key": srv_key_path
    }

def run_test_server(handler_fn, srv_crt, srv_key, ready_event, stop_event, port_holder):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("127.0.0.1", 0))
    sock.listen(1)
    port_holder[0] = sock.getsockname()[1]
    sock.settimeout(1.0)
    ready_event.set()

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile=srv_crt, keyfile=srv_key)

    try:
        while not stop_event.is_set():
            try:
                conn, _ = sock.accept()
            except socket.timeout:
                continue
            try:
                handler_fn(conn, ctx)
            finally:
                try:
                    conn.close()
                except Exception:
                    pass
            break
    finally:
        sock.close()

def safe_unwrap(tls_conn):
    try:
        tls_conn.unwrap()
    except Exception:
        pass

def main():
    print("======================================================================")
    print("  FortressOS Phase TLS-4: HTTPS Client Extension in /bin/wget Suite   ")
    print("======================================================================")

    # 1. Build host test binary
    print("=== [1/3] Building wget_https_host under ASan/UBSan ===")
    os.makedirs(BUILD_DIR, exist_ok=True)
    compile_cmd = [
        "gcc",
        "-std=c11",
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
        "-I" + os.path.join(REPO_ROOT, "src", "fs"),
        "-I" + os.path.join(REPO_ROOT, "src", "lib", "tls"),
        "-I" + os.path.join(REPO_ROOT, "src", "lib", "tls", "bearssl", "inc"),
        "-I" + os.path.join(REPO_ROOT, "src", "lib", "tls", "bearssl", "src"),
        "-I" + os.path.join(REPO_ROOT, "user"),
        "-I" + os.path.join(REPO_ROOT, "tests"),
        "tests/wget_https_host.c",
        os.path.join(REPO_ROOT, "src", "lib", "tls", "anchor_parser.c"),
        LIB_TLS,
        "-o", BIN_OUT
    ]
    res = subprocess.run(compile_cmd)
    if res.returncode != 0:
        print("FAIL: Compilation of tests/wget_https_host.c failed")
        sys.exit(1)
    print("    -> Successfully built:", BIN_OUT)

    # 2. Setup PKI
    print("=== [2/3] Generating PKI test hierarchy ===")
    workdir = tempfile.mkdtemp(prefix="fortress_wget_https_")
    try:
        pki = generate_pki(workdir)
        env = os.environ.copy()
        env["FORTRESS_ANCHORS_PATH"] = pki["anchors_v1"]

        # 3. Test Cases
        print("=== [3/3] Executing HTTPS Client Verification Cases ===")

        # Case 1: Standard HTTPS Download with Content-Length
        def handler_case1(raw_sock, ctx):
            tls_conn = ctx.wrap_socket(raw_sock, server_side=True)
            _ = tls_conn.recv(4096)
            body = b"Hello FortressOS HTTPS Content-Length\n"
            resp = b"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body
            tls_conn.sendall(resp)
            safe_unwrap(tls_conn)

        ready = threading.Event(); stop = threading.Event(); port = [0]
        t = threading.Thread(target=run_test_server, args=(handler_case1, pki["server_crt"], pki["server_key"], ready, stop, port))
        t.start(); ready.wait()
        out_f = os.path.join(workdir, "case1_out.txt")
        url = f"https://test.fortress.local:{port[0]}/case1"
        res = subprocess.run([BIN_OUT, "-q", url, "-O", out_f], env=env, capture_output=True, text=True)
        stop.set(); t.join()
        assert res.returncode == 0, f"Case 1 failed: {res.stderr}"
        assert os.path.exists(out_f), "Case 1 output file does not exist"
        with open(out_f, "rb") as f:
            assert f.read() == b"Hello FortressOS HTTPS Content-Length\n"
        print("  [PASS] Case 1: Standard HTTPS download with Content-Length & atomic rename")

        # Case 2: HTTPS Download with Chunked Transfer Encoding
        def handler_case2(raw_sock, ctx):
            tls_conn = ctx.wrap_socket(raw_sock, server_side=True)
            _ = tls_conn.recv(4096)
            resp = b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" \
                   b"7\r\nChunk1 \r\n" \
                   b"8\r\nChunk2!!\r\n" \
                   b"0\r\n\r\n"
            tls_conn.sendall(resp)
            safe_unwrap(tls_conn)

        ready = threading.Event(); stop = threading.Event(); port = [0]
        t = threading.Thread(target=run_test_server, args=(handler_case2, pki["server_crt"], pki["server_key"], ready, stop, port))
        t.start(); ready.wait()
        out_f = os.path.join(workdir, "case2_out.txt")
        url = f"https://test.fortress.local:{port[0]}/case2"
        res = subprocess.run([BIN_OUT, "-q", url, "-O", out_f], env=env, capture_output=True, text=True)
        stop.set(); t.join()
        assert res.returncode == 0, f"Case 2 failed: {res.stderr}"
        assert os.path.exists(out_f), "Case 2 output file does not exist"
        with open(out_f, "rb") as f:
            assert f.read() == b"Chunk1 Chunk2!!"
        print("  [PASS] Case 2: HTTPS chunked transfer encoding stream decoding")

        # Case 3: HTTPS -> HTTP Downgrade Redirect Refusal
        def handler_case3(raw_sock, ctx):
            tls_conn = ctx.wrap_socket(raw_sock, server_side=True)
            _ = tls_conn.recv(4096)
            resp = b"HTTP/1.0 301 Moved Permanently\r\nLocation: http://insecure.example.com/steal\r\n\r\n"
            tls_conn.sendall(resp)
            safe_unwrap(tls_conn)

        ready = threading.Event(); stop = threading.Event(); port = [0]
        t = threading.Thread(target=run_test_server, args=(handler_case3, pki["server_crt"], pki["server_key"], ready, stop, port))
        t.start(); ready.wait()
        out_f = os.path.join(workdir, "case3_out.txt")
        url = f"https://test.fortress.local:{port[0]}/case3"
        res = subprocess.run([BIN_OUT, "-q", url, "-O", out_f], env=env, capture_output=True, text=True)
        stop.set(); t.join()
        assert res.returncode != 0, "Case 3 should have failed on HTTPS->HTTP downgrade"
        assert not os.path.exists(out_f), "Case 3 output file should NOT have been created"
        print("  [PASS] Case 3: HTTPS to HTTP downgrade redirect rejected")

        # Case 4: HTTPS -> HTTPS Valid Redirect
        def handler_case4_s1(raw_sock, ctx, target_port):
            tls_conn = ctx.wrap_socket(raw_sock, server_side=True)
            _ = tls_conn.recv(4096)
            resp = b"HTTP/1.0 302 Found\r\nLocation: https://test.fortress.local:" + str(target_port).encode() + b"/final\r\n\r\n"
            tls_conn.sendall(resp)
            safe_unwrap(tls_conn)

        def handler_case4_s2(raw_sock, ctx):
            tls_conn = ctx.wrap_socket(raw_sock, server_side=True)
            _ = tls_conn.recv(4096)
            body = b"Redirect Successful!\n"
            resp = b"HTTP/1.0 200 OK\r\nContent-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body
            tls_conn.sendall(resp)
            safe_unwrap(tls_conn)

        ready2 = threading.Event(); stop2 = threading.Event(); port2 = [0]
        t2 = threading.Thread(target=run_test_server, args=(handler_case4_s2, pki["server_crt"], pki["server_key"], ready2, stop2, port2))
        t2.start(); ready2.wait()

        ready1 = threading.Event(); stop1 = threading.Event(); port1 = [0]
        t1 = threading.Thread(target=run_test_server, args=(lambda s, c: handler_case4_s1(s, c, port2[0]), pki["server_crt"], pki["server_key"], ready1, stop1, port1))
        t1.start(); ready1.wait()

        out_f = os.path.join(workdir, "case4_out.txt")
        url = f"https://test.fortress.local:{port1[0]}/initial"
        res = subprocess.run([BIN_OUT, "-q", url, "-O", out_f], env=env, capture_output=True, text=True)
        stop1.set(); t1.join()
        stop2.set(); t2.join()
        assert res.returncode == 0, f"Case 4 failed: {res.stderr}"
        with open(out_f, "rb") as f:
            assert f.read() == b"Redirect Successful!\n"
        print("  [PASS] Case 4: HTTPS to HTTPS redirect followed and revalidated")

        # Case 5: HTTP 206 Partial Content Rejection
        def handler_case5(raw_sock, ctx):
            tls_conn = ctx.wrap_socket(raw_sock, server_side=True)
            _ = tls_conn.recv(4096)
            resp = b"HTTP/1.0 206 Partial Content\r\nContent-Range: bytes 0-4/10\r\n\r\nhello"
            tls_conn.sendall(resp)
            safe_unwrap(tls_conn)

        ready = threading.Event(); stop = threading.Event(); port = [0]
        t = threading.Thread(target=run_test_server, args=(handler_case5, pki["server_crt"], pki["server_key"], ready, stop, port))
        t.start(); ready.wait()
        out_f = os.path.join(workdir, "case5_out.txt")
        url = f"https://test.fortress.local:{port[0]}/case5"
        res = subprocess.run([BIN_OUT, "-q", url, "-O", out_f], env=env, capture_output=True, text=True)
        stop.set(); t.join()
        assert res.returncode != 0, "Case 5 should have failed on HTTP 206"
        assert not os.path.exists(out_f), "Case 5 output file should NOT have been created"
        print("  [PASS] Case 5: Unrequested HTTP 206 Partial Content rejected")

        # Case 6: Missing Authenticated close_notify / Abrupt TCP Drop
        def handler_case6(raw_sock, ctx):
            tls_conn = ctx.wrap_socket(raw_sock, server_side=True)
            _ = tls_conn.recv(4096)
            resp = b"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nIncomplete Data"
            tls_conn.sendall(resp)
            # Abrupt drop: close raw socket without sending TLS close_notify
            raw_sock.close()

        ready = threading.Event(); stop = threading.Event(); port = [0]
        t = threading.Thread(target=run_test_server, args=(handler_case6, pki["server_crt"], pki["server_key"], ready, stop, port))
        t.start(); ready.wait()
        out_f = os.path.join(workdir, "case6_out.txt")
        url = f"https://test.fortress.local:{port[0]}/case6"
        res = subprocess.run([BIN_OUT, "-q", url, "-O", out_f], env=env, capture_output=True, text=True)
        stop.set(); t.join()
        assert res.returncode != 0, "Case 6 should have failed on missing close_notify"
        assert not os.path.exists(out_f), "Case 6 output file should NOT have been created on abrupt drop"
        print("  [PASS] Case 6: Mandatory authenticated close_notify teardown commit condition")

        # Case 7: Content-Length Truncation
        def handler_case7(raw_sock, ctx):
            tls_conn = ctx.wrap_socket(raw_sock, server_side=True)
            _ = tls_conn.recv(4096)
            # Declares 50 bytes, only sends 15 bytes
            resp = b"HTTP/1.0 200 OK\r\nContent-Length: 50\r\n\r\nShort content.."
            tls_conn.sendall(resp)
            safe_unwrap(tls_conn)

        ready = threading.Event(); stop = threading.Event(); port = [0]
        t = threading.Thread(target=run_test_server, args=(handler_case7, pki["server_crt"], pki["server_key"], ready, stop, port))
        t.start(); ready.wait()
        out_f = os.path.join(workdir, "case7_out.txt")
        url = f"https://test.fortress.local:{port[0]}/case7"
        res = subprocess.run([BIN_OUT, "-q", url, "-O", out_f], env=env, capture_output=True, text=True)
        stop.set(); t.join()
        assert res.returncode != 0, "Case 7 should have failed on truncated Content-Length"
        assert not os.path.exists(out_f), "Case 7 output file should NOT have been created"
        print("  [PASS] Case 7: Content-Length truncation rejected")

        # Case 8: Stdout Streaming (-O -)
        def handler_case8(raw_sock, ctx):
            tls_conn = ctx.wrap_socket(raw_sock, server_side=True)
            _ = tls_conn.recv(4096)
            body = b"STDOUT STREAMING SUCCESS"
            resp = b"HTTP/1.0 200 OK\r\nContent-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body
            tls_conn.sendall(resp)
            safe_unwrap(tls_conn)

        ready = threading.Event(); stop = threading.Event(); port = [0]
        t = threading.Thread(target=run_test_server, args=(handler_case8, pki["server_crt"], pki["server_key"], ready, stop, port))
        t.start(); ready.wait()
        url = f"https://test.fortress.local:{port[0]}/case8"
        res = subprocess.run([BIN_OUT, "-q", url, "-O", "-"], env=env, capture_output=True, text=True)
        stop.set(); t.join()
        assert res.returncode == 0, f"Case 8 failed: {res.stderr}"
        assert "STDOUT STREAMING SUCCESS" in res.stdout, "Case 8 stdout did not receive body"
        print("  [PASS] Case 8: Stdout streaming (-O -) without temporary file")

        # Case 9: Cumulative 10 MiB Session Limit Enforcement
        def handler_case9(raw_sock, ctx):
            tls_conn = ctx.wrap_socket(raw_sock, server_side=True)
            _ = tls_conn.recv(4096)
            resp_hdr = b"HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\n\r\n"
            tls_conn.sendall(resp_hdr)
            chunk = b"A" * 65536
            # Try to send 12 MiB
            for _ in range(192):
                try:
                    tls_conn.sendall(chunk)
                except Exception:
                    break
            try:
                safe_unwrap(tls_conn)
            except Exception:
                pass

        ready = threading.Event(); stop = threading.Event(); port = [0]
        t = threading.Thread(target=run_test_server, args=(handler_case9, pki["server_crt"], pki["server_key"], ready, stop, port))
        t.start(); ready.wait()
        out_f = os.path.join(workdir, "case9_out.bin")
        url = f"https://test.fortress.local:{port[0]}/case9"
        res = subprocess.run([BIN_OUT, "-q", url, "-O", out_f], env=env, capture_output=True, text=True)
        stop.set(); t.join()
        assert res.returncode != 0, "Case 9 should have failed on 10 MiB limit exceeded"
        assert not os.path.exists(out_f), "Case 9 output file should NOT exist"
        print("  [PASS] Case 9: Cumulative 10 MiB session payload limit enforcement")

        # Case 10: Missing / Malformed anchors.v1 Fail-Closed
        bad_env = os.environ.copy()
        bad_env["FORTRESS_ANCHORS_PATH"] = "/nonexistent/anchors.v1"
        out_f = os.path.join(workdir, "case10_out.txt")
        url = "https://test.fortress.local:9999/test"
        res = subprocess.run([BIN_OUT, "-q", url, "-O", out_f], env=bad_env, capture_output=True, text=True)
        assert res.returncode != 0, "Case 10 should have failed immediately on missing anchors"
        assert not os.path.exists(out_f), "Case 10 output file should NOT exist"
        print("  [PASS] Case 10: Missing / malformed anchors.v1 fail-closed rejection")

        print("\n>>> ALL PHASE TLS-4 HTTPS CLIENT TESTS PASSED (100%) <<<")

    finally:
        shutil.rmtree(workdir, ignore_errors=True)

if __name__ == "__main__":
    main()
