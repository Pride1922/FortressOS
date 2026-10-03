#!/usr/bin/env python3
"""Acceptance test for /bin/wget under QEMU.

Runs BIOS and UEFI boot with e1000 under SMP=1 with a disposable ext2 partition at /mnt,
spins up a local host HTTP test server, and executes /bin/wget tests:
  1. /bin/wget --help
  2. HTTPS rejection
  3. Plain HTTP download (hello.txt saved to /mnt)
  4. Custom output path (-O /mnt/data.bin, 2048 bytes)
  5. Stdout streaming (-O - | wc -c, 24 bytes)
  6. 302 Redirect following (/redir -> /hello.txt)
  7. 404 Not Found error handling
  8. Content-Length mismatch detection (server sends fewer bytes than promised)
Powers off cleanly. Enforces argv preflight (disposable ISO/OVMF, disposable NVMe ext2 disk).
"""

from pathlib import Path
import http.server
import os
import re
import shutil
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading
import time

REPO = Path(__file__).resolve().parent.parent
CODE = Path("/usr/share/OVMF/OVMF_CODE_4M.fd")
VARS = Path("/usr/share/OVMF/OVMF_VARS_4M.fd")
ISO = REPO / "bin" / "fortress.iso"
NVME_SRC = REPO / "build" / "nvme_gpt.img"
PROMPT_PATTERN = r"(?:fortress> |(?:\[-?\d+\] )?fortress:[^\r\n]* \$ |\[[a-zA-Z0-9_\-\./]+\]# )"

SERVER_PORT = 18080


class TestHTTPHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass  # Suppress console logging

    def do_GET(self):
        if self.path == "/hello.txt":
            body = b"Hello FortressOS World!\n"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/data.bin":
            body = bytes([i % 256 for i in range(2048)])
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/redir":
            self.send_response(302)
            self.send_header("Location", "/hello.txt")
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
        elif self.path == "/notfound":
            body = b"Not Found"
            self.send_response(404)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/mismatch":
            # Header claims 100 bytes, but sends only 35 bytes
            short_body = b"Short body: only thirty-five bytes."
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", "100")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(short_body)
        else:
            self.send_response(404)
            self.end_headers()


def start_http_server(port):
    server = socketserver.TCPServer(("0.0.0.0", port), TestHTTPHandler)
    server.allow_reuse_address = True
    th = threading.Thread(target=server.serve_forever, daemon=True)
    th.start()
    return server


def qemu_command(mode, iso, variables, log, uart_path, nvme_path):
    cmd = [
        "qemu-system-x86_64", "-accel", "tcg", "-M", "q35", "-m", "2G",
        "-smp", "1", "-display", "none", "-monitor", "none", "-no-reboot",
        "-boot", "d", "-cdrom", str(iso),
        "-netdev", "user,id=net0",
        "-device", "e1000,netdev=net0",
        "-drive", f"file={nvme_path},if=none,id=nvm0,format=raw,snapshot=off",
        "-device", "nvme,serial=fortress0,drive=nvm0",
        "-fw_cfg", "name=opt/fortress/write_test,string=1",
        "-chardev", f"socket,id=uart,path={uart_path},server=on,wait=off,logfile={log}",
        "-serial", "chardev:uart"
    ]
    if mode == "uefi":
        cmd += [
            "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={CODE}",
            "-drive", f"if=pflash,format=raw,unit=1,file={variables}"
        ]
    return cmd


def preflight(cmd, mode, iso, variables, log, uart_path, nvme_path):
    assert mode in ("bios", "uefi")
    expected = qemu_command(mode, iso, variables, log, uart_path, nvme_path)
    assert cmd == expected, f"unexpected QEMU argv: {cmd} != {expected}"
    for token in ("-hda", "-hdb", "-blockdev", "usb-storage"):
        assert token not in cmd, f"preflight found forbidden token {token}"


def run_session(mode, iso, tmp, port):
    log = REPO / "build" / f"wget-{mode}.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text("")
    uart_path = tmp / f"uart-{mode}"
    variables = tmp / f"vars-{mode}.fd"
    nvme_path = tmp / f"nvme-{mode}.img"
    shutil.copyfile(NVME_SRC, nvme_path)

    if mode == "uefi":
        shutil.copyfile(VARS, variables)

    cmd = qemu_command(mode, iso, variables, log, uart_path, nvme_path)
    preflight(cmd, mode, iso, variables, log, uart_path, nvme_path)

    with log.with_suffix(".stderr").open("wb") as stderr:
        proc = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=stderr)
        uart = None
        stop_reader = threading.Event()
        try:
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                if uart_path.exists():
                    try:
                        uart = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                        uart.connect(str(uart_path))
                        break
                    except (ConnectionRefusedError, OSError):
                        pass
                time.sleep(0.05)
            assert uart is not None, f"Could not connect to UART at {uart_path}"
            uart.settimeout(0.2)

            def drain():
                while not stop_reader.is_set():
                    try:
                        if not uart.recv(65536):
                            return
                    except (socket.timeout, OSError):
                        pass
            reader = threading.Thread(target=drain, daemon=True)
            reader.start()

            def get_text():
                return re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "", log.read_bytes().decode(errors="replace")).replace("\r", "")

            def wait_for_prompt(after_idx=0, timeout=60):
                d = time.monotonic() + timeout
                while time.monotonic() < d:
                    t = get_text()
                    if re.search(PROMPT_PATTERN, t[after_idx:]):
                        return t[after_idx:]
                    time.sleep(0.1)
                raise TimeoutError(f"Prompt not seen within {timeout}s: {t[-500:]}")

            def run_command(command, timeout=20):
                before = len(get_text())
                uart.sendall(command.encode("ascii") + b"\n")
                wait_for_prompt(after_idx=before, timeout=timeout)
                return get_text()[before:]

            # 1. Wait for initial Ring 3 shell prompt
            initial = wait_for_prompt()
            print(f"[{mode}] Shell ready")

            # 2. Switch to writable persistent partition /mnt
            run_command("cd /mnt")
            print(f"[{mode}] Working directory set to /mnt")

            # 3. Test /bin/wget --help
            out = run_command("/bin/wget --help")
            assert "usage: wget" in out, f"wget --help failed: {out}"
            print(f"[{mode}] Case 1 (wget --help): PASS")

            # 4. Test HTTPS rejection
            out = run_command("/bin/wget https://neverssl.com/")
            assert "HTTPS (TLS) is not supported yet" in out, f"HTTPS rejection failed: {out}"
            print(f"[{mode}] Case 2 (HTTPS rejection): PASS")

            # 5. Plain HTTP download of hello.txt (with TCP reboot quiet time warmup)
            warmup = time.monotonic() + 180
            while True:
                out = run_command(f"/bin/wget http://10.0.2.2:{port}/hello.txt")
                if "error 21" not in out:
                    break
                assert time.monotonic() < warmup, "reboot quiet period did not expire"
                time.sleep(5)
            assert "200 OK" in out, f"Download failed: {out}"
            assert "'hello.txt' saved" in out, f"Save message missing: {out}"
            out_cat = run_command("cat hello.txt")
            assert "Hello FortressOS World!" in out_cat, f"Content verification failed: {out_cat}"
            print(f"[{mode}] Case 3 (plain HTTP download & content match): PASS")

            # 6. Custom output path: -O /mnt/data.bin
            out = run_command(f"/bin/wget http://10.0.2.2:{port}/data.bin -O /mnt/data.bin")
            assert "200 OK" in out, f"Custom out download failed: {out}"
            assert "'/mnt/data.bin' saved [2048/2048]" in out, f"Save 2048 missing: {out}"
            out_wc = run_command("wc -c /mnt/data.bin")
            assert "2048" in out_wc, f"wc byte count mismatch: {out_wc}"
            print(f"[{mode}] Case 4 (custom output path -O & 2048 bytes): PASS")

            # 7. Stdout streaming: -O -
            out = run_command(f"/bin/wget -q http://10.0.2.2:{port}/hello.txt -O - | wc -c")
            assert "24" in out, f"Piped byte count mismatch (expected 24): {out}"
            print(f"[{mode}] Case 5 (quiet stdout stream -O - | wc -c): PASS")

            # 8. 302 Redirect following
            out = run_command(f"/bin/wget http://10.0.2.2:{port}/redir -O /mnt/redir.txt")
            assert "Location: /hello.txt [following]" in out, f"Redirect log missing: {out}"
            assert "200 OK" in out, f"Redirect target download failed: {out}"
            out_cat = run_command("cat /mnt/redir.txt")
            assert "Hello FortressOS World!" in out_cat, f"Redirected content mismatch: {out_cat}"
            print(f"[{mode}] Case 6 (302 redirect following): PASS")

            # 9. 404 Not Found error handling
            out = run_command(f"/bin/wget http://10.0.2.2:{port}/notfound")
            assert "server returned error: HTTP 404" in out or "404 Not Found" in out, f"404 handling failed: {out}"
            out_status = run_command("echo $?")
            assert "1" in out_status, f"Expected non-zero exit code: {out_status}"
            print(f"[{mode}] Case 7 (404 Not Found status handling & exit 1): PASS")

            # 10. Content-Length mismatch detection
            out = run_command(f"/bin/wget http://10.0.2.2:{port}/mismatch -O /mnt/mismatch.txt")
            assert "read error: expected 100 bytes, got 35" in out, f"Mismatch detection failed: {out}"
            out_status = run_command("echo $?")
            assert "1" in out_status, f"Expected non-zero exit code on mismatch: {out_status}"
            print(f"[{mode}] Case 8 (Content-Length mismatch detection & exit 1): PASS")

            # 11. Clean poweroff
            uart.sendall(b"poweroff\n")
            proc.wait(timeout=10)
            print(f"[{mode}] Poweroff clean")

        finally:
            stop_reader.set()
            if uart:
                try:
                    uart.close()
                except OSError:
                    pass
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()


def main():
    if not ISO.exists():
        print(f"Missing {ISO}; run 'make' first.")
        sys.exit(1)
    if not NVME_SRC.exists():
        print(f"Missing {NVME_SRC}; run 'make nvme-gpt-disk' first.")
        sys.exit(1)

    print(f"Starting host HTTP test server on port {SERVER_PORT}...")
    server = start_http_server(SERVER_PORT)

    modes = ["bios", "uefi"] if CODE.exists() and VARS.exists() else ["bios"]
    with tempfile.TemporaryDirectory() as tmp_str:
        tmp = Path(tmp_str)
        for mode in modes:
            print(f"\n=== Running /bin/wget test under {mode.upper()} ===")
            run_session(mode, ISO, tmp, SERVER_PORT)

    server.shutdown()
    print("\n>>> ALL WGET QEMU TESTS PASSED (100%) <<<")


if __name__ == "__main__":
    main()
