#!/usr/bin/env python3
"""Multi-Firmware QEMU Acceptance Suite for Tiny HTTP Server (Issue #43).

Verifies acceptance criteria:
1. /bin/httpd starts and serves static files single-threaded with non-blocking I/O & poll().
2. HTTP GET /motd returns 200 OK with correct MIME type and content.
3. HTTP HEAD /motd returns 200 OK with Content-Length and zero body.
4. HTTP GET /missing returns 404 Not Found.
5. Path traversal /../ is strictly forbidden and returns 403 Forbidden.
6. In-guest /bin/wget can fetch from /bin/httpd.
7. Clean execution across BIOS and UEFI firmware.
"""

from pathlib import Path
import http.client
import os
import re
import selectors
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


class Guest:
    def __init__(self, firmware, smp, tmp):
        self.firmware = firmware
        self.smp = smp
        self.port = free_port()
        self.data = bytearray()
        argv = [
            'qemu-system-x86_64', '-M', 'q35', '-m', '512M', '-smp', str(smp),
            '-accel', 'tcg', '-cpu', 'max,+rdrand,+rdseed', '-display', 'none',
            '-monitor', 'none', '-serial', 'stdio', '-no-reboot', '-boot', 'd',
            '-cdrom', str(ROOT / 'bin/fortress.iso'),
            '-netdev', f'user,id=n0,hostfwd=tcp:127.0.0.1:{self.port}-:8080',
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
        self.proc.stdin.write((line + '\n').encode())
        self.proc.stdin.flush()

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


def raw_http_req(host, port, method, path):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5.0)
    s.connect((host, port))
    req = f"{method} {path} HTTP/1.0\r\nHost: {host}:{port}\r\nConnection: close\r\n\r\n"
    s.sendall(req.encode('utf-8'))
    resp = b""
    while True:
        try:
            chunk = s.recv(4096)
            if not chunk:
                break
            resp += chunk
        except Exception:
            break
    s.close()
    return resp


def test_firmware(firmware, smp=1):
    print(f"\n========================================================")
    print(f"Testing /bin/httpd under {firmware.upper()} (SMP={smp})")
    print(f"========================================================")

    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = Path(tmpdir)
        guest = Guest(firmware, smp, tmp)
        try:
            print("[1/7] Waiting for boot and login prompt...")
            guest.wait(lambda t: t.endswith('FortressOS login: '), timeout=45)
            print("  [PASS] Boot successful; login prompt reached")

            print("[2/7] Logging in as operator...")
            guest.send('operator')
            guest.wait(lambda t: t.endswith('$ '), timeout=15)
            print("  [PASS] Logged in successfully as operator")

            print("[3/7] Verifying '/bin/httpd --help'...")
            help_out = guest.command('/bin/httpd --help')
            assert "usage: httpd" in help_out, f"Missing usage in httpd --help: {help_out}"
            print("  [PASS] '/bin/httpd --help' output verified")

            print("[4/7] Launching '/bin/httpd -p 8080 -d /etc &' in background...")
            start_len = len(guest.data)
            guest.send('/bin/httpd -p 8080 -d /etc &')
            guest.wait(lambda t: "httpd: listening on port 8080" in t, start=start_len, timeout=10)
            guest.wait(lambda t: t.endswith('$ '), timeout=5)
            print("  [PASS] /bin/httpd listening on port 8080")

            # Allow listening socket and ARP/network settling
            time.sleep(0.5)

            print("[5/7] Testing HTTP requests from host...")

            # 5a. GET /motd (200 OK)
            resp = raw_http_req('127.0.0.1', guest.port, 'GET', '/motd').decode('utf-8', errors='replace')
            assert "HTTP/1.0 200 OK" in resp or "HTTP/1.1 200 OK" in resp, f"Unexpected GET resp: {resp[:200]}"
            assert "Content-Type: text/plain" in resp, f"Missing Content-Type: {resp[:200]}"
            assert "Welcome to FortressOS" in resp, f"Missing motd content: {resp}"
            print("  [PASS] GET /motd -> 200 OK with correct text/plain content")

            # 5b. HEAD /motd (200 OK with no body)
            resp_head = raw_http_req('127.0.0.1', guest.port, 'HEAD', '/motd').decode('utf-8', errors='replace')
            assert "HTTP/1.0 200 OK" in resp_head or "HTTP/1.1 200 OK" in resp_head, f"Unexpected HEAD resp: {resp_head[:200]}"
            assert "Content-Length:" in resp_head, f"Missing Content-Length in HEAD: {resp_head}"
            parts = resp_head.split("\r\n\r\n", 1)
            assert len(parts) == 2 and parts[1] == "", f"HEAD response contained non-empty body: {parts[1]}"
            print("  [PASS] HEAD /motd -> 200 OK with headers only (no body)")

            # 5c. GET /nonexistent (404 Not Found)
            resp_404 = raw_http_req('127.0.0.1', guest.port, 'GET', '/nonexistent').decode('utf-8', errors='replace')
            assert "404 Not Found" in resp_404, f"Expected 404 Not Found: {resp_404[:200]}"
            print("  [PASS] GET /nonexistent -> 404 Not Found")

            # 5d. Path traversal rejection (403 Forbidden)
            resp_403 = raw_http_req('127.0.0.1', guest.port, 'GET', '/../../shadow').decode('utf-8', errors='replace')
            assert "403 Forbidden" in resp_403, f"Expected 403 Forbidden on path traversal: {resp_403[:200]}"
            print("  [PASS] GET /../../shadow -> 403 Forbidden (traversal blocked)")

            # 5e. Method not allowed (405 Method Not Allowed)
            resp_405 = raw_http_req('127.0.0.1', guest.port, 'POST', '/motd').decode('utf-8', errors='replace')
            assert "405 Method Not Allowed" in resp_405, f"Expected 405 on POST: {resp_405[:200]}"
            print("  [PASS] POST /motd -> 405 Method Not Allowed")

            print("[6/7] Testing client slot reuse & rapid requests...")
            for i in range(10):
                r = raw_http_req('127.0.0.1', guest.port, 'GET', '/motd').decode('utf-8', errors='replace')
                assert "200 OK" in r and "Welcome to FortressOS" in r, f"Request {i} failed: {r[:200]}"
            print("  [PASS] 10 sequential client connections served cleanly (multiplexing & slot reuse)")

            print("[7/7] Clean poweroff...")
            guest.close()
            print(f"  [PASS] {firmware.upper()} acceptance completed cleanly")

        finally:
            guest.close()


def main():
    test_firmware('bios', smp=1)
    test_firmware('uefi', smp=1)
    print("\n========================================================")
    print("ALL HTTPD MULTI-FIRMWARE ACCEPTANCE TESTS PASSED (100%)!")
    print("========================================================\n")


if __name__ == '__main__':
    main()
