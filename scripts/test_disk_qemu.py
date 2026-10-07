#!/usr/bin/env python3
"""QEMU Integration Test for /bin/disk (Checkpoints 1 & 2: disk usage & disk list)."""
from pathlib import Path
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import re

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))
from test_nmi_transitions import connect


def strip_ansi(s):
    return re.sub(r'\x1b\[[0-9;?]*[a-zA-Z]', '', s)


def send_str(uart, s):
    for b in s.encode():
        uart.send(bytes([b]))
        time.sleep(0.005)


def wait_for_pattern(log_path, pattern, child, start_pos=0, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if log_path.exists():
            txt = log_path.read_text(errors="replace").replace("\r", "")
            if pattern in txt[start_pos:] or (pattern == "fortress:/ $ " and ":/ $ " in txt[start_pos:]):
                return txt
        assert child.poll() is None, f"QEMU exited prematurely with code {child.poll()}"
        time.sleep(0.05)
    full = log_path.read_text(errors="replace") if log_path.exists() else ""
    raise AssertionError(f"Timeout waiting for {pattern!r} after pos {start_pos}. Log tail:\n{full[-1000:]}")


def test_disk_qemu():
    print("=== Running disk QEMU Integration Test ===")
    build_dir = REPO / "build"
    nvme_img = build_dir / "nvme_gpt.img"
    if not nvme_img.exists():
        alt_img = Path("/home/fabio/fortress-obs-build/build/nvme_gpt.img")
        if not alt_img.exists():
            alt_img = Path("/mnt/c/Sources/FortressOS/build/nvme_gpt.img")
            if alt_img.exists():
                shutil.copyfile(alt_img, nvme_img)

    with tempfile.TemporaryDirectory(prefix="fortress-disk-qemu-") as tmp:
        uart_path = Path(tmp) / "uart"
        log_path = REPO / "build" / "disk_serial.log"
        if log_path.exists(): log_path.unlink()

        qemu_cmd = [
            "qemu-system-x86_64", "-M", "q35", "-m", "2G", "-smp", "1", "-display", "none",
            "-no-reboot", "-boot", "d", "-cdrom", "bin/fortress.iso",
            "-chardev", f"socket,id=uart,path={uart_path},server=on,wait=off,logfile={log_path}",
            "-serial", "chardev:uart"
        ]
        if nvme_img.exists():
            qemu_cmd.extend([
                "-drive", f"file={nvme_img},if=none,id=nvm0,format=raw,snapshot=on",
                "-device", "nvme,serial=fortress0,drive=nvm0",
                "-fw_cfg", "name=opt/fortress/write_test,string=1"
            ])

        proc = subprocess.Popen(qemu_cmd, cwd=REPO)
        try:
            uart = connect(uart_path)
            stop_drain = threading.Event()
            def drain():
                uart.settimeout(0.2)
                while not stop_drain.is_set():
                    try:
                        if not uart.recv(4096): return
                    except (socket.timeout, OSError):
                        continue
            threading.Thread(target=drain, daemon=True).start()

            print("  [1] Waiting for interactive shell prompt...")
            wait_for_pattern(log_path, "fortress:/ $ ", proc, timeout=40)
            print("  [1] Shell prompt ready.")

            # Test 1: disk --help
            print("  [2] Testing: disk --help")
            pos = len(log_path.read_text(errors="replace"))
            send_str(uart, "disk --help\n")
            wait_for_pattern(log_path, "fortress:/ $ ", proc, start_pos=pos, timeout=10)
            out = strip_ansi(log_path.read_text(errors="replace")[pos:])
            assert "Usage: disk" in out
            print("  [PASS] disk --help verified.")

            # Test 2: disk (default invocation -> disk list)
            print("  [3] Testing: disk (default invocation -> disk list)")
            pos = len(log_path.read_text(errors="replace"))
            cmd2 = "disk\n"
            send_str(uart, cmd2)
            wait_for_pattern(log_path, "fortress:/ $ ", proc, start_pos=pos + len(cmd2) - 5, timeout=20)
            out_def = strip_ansi(log_path.read_text(errors="replace")[pos:])
            print(f"      Default (list) Output:\n{out_def.strip()}\n")
            assert "NAME" in out_def, f"Missing NAME header: {out_def}"
            assert "SIZE" in out_def, f"Missing SIZE header: {out_def}"
            assert "SECTOR" in out_def, f"Missing SECTOR header: {out_def}"
            assert "MOUNT" in out_def, f"Missing MOUNT header: {out_def}"
            assert "initramfs" in out_def, f"Missing initramfs in list: {out_def}"
            assert "nvme0n1" in out_def, f"Missing nvme0n1 in list: {out_def}"
            print("  [PASS] disk (default invocation -> disk list) verified.")

            # Test 3: disk list -c (comparison format)
            print("  [4] Testing: disk list -c")
            pos = len(log_path.read_text(errors="replace"))
            cmd3 = "disk list -c\n"
            send_str(uart, cmd3)
            wait_for_pattern(log_path, "fortress:/ $ ", proc, start_pos=pos + len(cmd3) - 5, timeout=20)
            out_list_cmp = strip_ansi(log_path.read_text(errors="replace")[pos:])
            print(f"      List Comparison Output:\n{out_list_cmp.strip()}\n")
            assert "name=initramfs sector=512" in out_list_cmp, f"Missing initramfs in list -c: {out_list_cmp}"
            assert "mount=/" in out_list_cmp, f"Missing mount=/ in list -c: {out_list_cmp}"
            assert "name=nvme0n1" in out_list_cmp, f"Missing nvme0n1 in list -c: {out_list_cmp}"
            print("  [PASS] disk list -c verified.")

            # Test 4: disk usage (table format)
            print("  [5] Testing: disk usage")
            pos = len(log_path.read_text(errors="replace"))
            cmd4 = "disk usage\n"
            send_str(uart, cmd4)
            wait_for_pattern(log_path, "fortress:/ $ ", proc, start_pos=pos + len(cmd4) - 5, timeout=20)
            out_tbl = strip_ansi(log_path.read_text(errors="replace")[pos:])
            print(f"      Table Output:\n{out_tbl.strip()}\n")
            assert "MOUNT" in out_tbl, f"Missing MOUNT header: {out_tbl}"
            assert "SOURCE" in out_tbl, f"Missing SOURCE header: {out_tbl}"
            assert "FS" in out_tbl, f"Missing FS header: {out_tbl}"
            assert "TarFS" in out_tbl, f"Missing TarFS entry: {out_tbl}"
            print("  [PASS] disk usage (table) verified.")

            # Test 5: disk usage -c (comparison format)
            print("  [6] Testing: disk usage -c")
            pos = len(log_path.read_text(errors="replace"))
            cmd5 = "disk usage -c\n"
            send_str(uart, cmd5)
            wait_for_pattern(log_path, "fortress:/ $ ", proc, start_pos=pos + len(cmd5) - 5, timeout=20)
            out_cmp = strip_ansi(log_path.read_text(errors="replace")[pos:])
            print(f"      Comparison Output:\n{out_cmp.strip()}\n")
            assert "mount=/" in out_cmp, f"Missing mount=/ in comparison output: {out_cmp}"
            assert "fs=TarFS" in out_cmp, f"Missing fs=TarFS in comparison output: {out_cmp}"
            print("  [PASS] disk usage -c (comparison) verified.")

            # Test 6: disk bench (standard run)
            print("  [7] Testing: disk bench -w 64K -n 10 /mnt")
            pos = len(log_path.read_text(errors="replace"))
            cmd6 = "disk bench -w 64K -n 10 /mnt\n"
            send_str(uart, cmd6)
            wait_for_pattern(log_path, "fortress:/ $ ", proc, start_pos=pos + len(cmd6) - 5, timeout=20)
            out_bench = strip_ansi(log_path.read_text(errors="replace")[pos:])
            print(f"      Bench Output:\n{out_bench.strip()}\n")
            assert "=== diskbench: /mnt ===" in out_bench, f"Missing bench header: {out_bench}"
            assert "Write: 65536 bytes" in out_bench, f"Missing write in bench: {out_bench}"
            assert "Read:  65536 bytes" in out_bench, f"Missing read in bench: {out_bench}"
            assert "Meta:  10 files" in out_bench, f"Missing meta in bench: {out_bench}"
            print("  [PASS] disk bench verified.")

            # Test 7: disk bench -c (comparison format)
            print("  [8] Testing: disk bench -c -w 64K -n 10 /mnt")
            pos = len(log_path.read_text(errors="replace"))
            cmd7 = "disk bench -c -w 64K -n 10 /mnt\n"
            send_str(uart, cmd7)
            wait_for_pattern(log_path, "fortress:/ $ ", proc, start_pos=pos + len(cmd7) - 5, timeout=20)
            out_bench_cmp = strip_ansi(log_path.read_text(errors="replace")[pos:])
            print(f"      Bench Comparison Output:\n{out_bench_cmp.strip()}\n")
            assert "diskbench context mount=/mnt" in out_bench_cmp, f"Missing context in bench -c: {out_bench_cmp}"
            assert "diskbench write bytes=65536" in out_bench_cmp, f"Missing write in bench -c: {out_bench_cmp}"
            assert "diskbench read bytes=65536" in out_bench_cmp, f"Missing read in bench -c: {out_bench_cmp}"
            assert "diskbench meta files=10" in out_bench_cmp, f"Missing meta in bench -c: {out_bench_cmp}"
            print("  [PASS] disk bench -c verified.")

            print("=== ALL DISK QEMU TESTS PASSED ===")

        finally:
            stop_drain.set()
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()


if __name__ == "__main__":
    test_disk_qemu()
