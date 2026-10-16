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
    return re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "", s)


def send_str(uart, s):
    uart.sendall(s.encode())



PROMPT_PATTERN = r"(?:fortress> |fortress:[^\n]* \$ |[A-Za-z0-9_-]+@[A-Za-z0-9-]+:[^\n]*[#$] )"


def get_clean_text(log_path):
    if not log_path.exists():
        return ""
    raw = log_path.read_bytes().decode(errors="replace")
    return strip_ansi(raw).replace("\r", "")


def wait_for_prompt(log_path, child, start_pos=0, timeout=30, uart=None, submitted=False):
    deadline = time.monotonic() + timeout
    sent_login = False
    while time.monotonic() < deadline:
        if log_path.exists():
            txt = get_clean_text(log_path)
            chunk = txt[start_pos:]
            if uart and not sent_login and "FortressOS login: " in chunk:
                sent_login = True
                uart.sendall(b"operator\n")
            if submitted:
                newline = chunk.find("\n")
                if newline >= 0:
                    start = newline + 1
                    m = re.search(r"(?:" + PROMPT_PATTERN + r")\s*\Z", chunk[start:])
                    if m:
                        return chunk
            else:
                m = re.search(r"(?:" + PROMPT_PATTERN + r")\s*\Z", chunk)
                if m:
                    return chunk
        assert child.poll() is None, f"QEMU exited prematurely with code {child.poll()}"
        time.sleep(0.05)
    full = get_clean_text(log_path)
    raise AssertionError(f"Timeout waiting for prompt after pos {start_pos}. Log tail:\n{full[-1000:]}")


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

            def exec_cmd(cmd_str, timeout=20):
                pos = len(get_clean_text(log_path))
                for b in (cmd_str + "\n").encode():
                    uart.sendall(bytes([b]))
                    time.sleep(0.005)
                chunk = wait_for_prompt(log_path, proc, start_pos=pos, timeout=timeout, submitted=True)
                nl = chunk.find("\n")
                return chunk[nl+1:] if nl >= 0 else chunk

            print("  [1] Waiting for interactive shell prompt...")
            wait_for_prompt(log_path, proc, start_pos=0, timeout=40, uart=uart, submitted=False)
            print("  [1] Shell prompt ready.")

            # Prepare /mnt permissions for diskbench tests
            exec_cmd("sudo chmod 777 /mnt")

            # Test 1: disk --help
            print("  [2] Testing: disk --help")
            out = exec_cmd("disk --help")
            assert "Usage: disk" in out
            print("  [PASS] disk --help verified.")

            # Test 2: disk (default invocation -> disk list)
            print("  [3] Testing: disk (default invocation -> disk list)")
            out_def = exec_cmd("disk")
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
            out_list_cmp = exec_cmd("disk list -c")
            print(f"      List Comparison Output:\n{out_list_cmp.strip()}\n")
            assert "name=initramfs sector=512" in out_list_cmp, f"Missing initramfs in list -c: {out_list_cmp}"
            assert "mount=/" in out_list_cmp, f"Missing mount=/ in list -c: {out_list_cmp}"
            assert "name=nvme0n1" in out_list_cmp, f"Missing nvme0n1 in list -c: {out_list_cmp}"
            print("  [PASS] disk list -c verified.")

            # Test 4: disk usage (table format)
            print("  [5] Testing: disk usage")
            out_tbl = exec_cmd("disk usage")
            print(f"      Table Output:\n{out_tbl.strip()}\n")
            assert "MOUNT" in out_tbl, f"Missing MOUNT header: {out_tbl}"
            assert "SOURCE" in out_tbl, f"Missing SOURCE header: {out_tbl}"
            assert "FS" in out_tbl, f"Missing FS header: {out_tbl}"
            assert "TarFS" in out_tbl, f"Missing TarFS entry: {out_tbl}"
            print("  [PASS] disk usage (table) verified.")

            # Test 5: disk usage -c (comparison format)
            print("  [6] Testing: disk usage -c")
            out_cmp = exec_cmd("disk usage -c")
            print(f"      Comparison Output:\n{out_cmp.strip()}\n")
            assert "mount=/" in out_cmp, f"Missing mount=/ in comparison output: {out_cmp}"
            assert "fs=TarFS" in out_cmp, f"Missing fs=TarFS in comparison output: {out_cmp}"
            print("  [PASS] disk usage -c (comparison) verified.")

            # Test 5b: df (alias/tool)
            print("  [6b] Testing: df")
            out_df = exec_cmd("df")
            print(f"      df Output:\n{out_df.strip()}\n")
            assert "MOUNT" in out_df, f"Missing MOUNT header in df: {out_df}"
            assert "AVAIL" in out_df, f"Missing AVAIL header in df: {out_df}"
            print("  [PASS] df verified.")

            # Test 6: disk bench (standard run)
            print("  [7] Testing: disk bench -w 64K -n 10 /mnt")
            out_bench = exec_cmd("disk bench -w 64K -n 10 /mnt")
            print(f"      Bench Output:\n{out_bench.strip()}\n")
            assert "=== diskbench: /mnt ===" in out_bench, f"Missing bench header: {out_bench}"
            assert "Write: 65536 bytes" in out_bench, f"Missing write in bench: {out_bench}"
            assert "Read:  65536 bytes" in out_bench, f"Missing read in bench: {out_bench}"
            assert "Meta:  10 files" in out_bench, f"Missing meta in bench: {out_bench}"
            print("  [PASS] disk bench verified.")

            # Test 7: disk bench -c (comparison format)
            print("  [8] Testing: disk bench -c -w 64K -n 10 /mnt")
            out_bench_cmp = exec_cmd("disk bench -c -w 64K -n 10 /mnt")
            print(f"      Bench Comparison Output:\n{out_bench_cmp.strip()}\n")
            assert "diskbench context mount=/mnt" in out_bench_cmp, f"Missing context in bench -c: {out_bench_cmp}"
            assert "diskbench write bytes=65536" in out_bench_cmp, f"Missing write in bench -c: {out_bench_cmp}"
            assert "diskbench read bytes=65536" in out_bench_cmp, f"Missing read in bench -c: {out_bench_cmp}"
            assert "diskbench meta files=10" in out_bench_cmp, f"Missing meta in bench -c: {out_bench_cmp}"
            print("  [PASS] disk bench -c verified.")

            # Test 8: disk info nvme0n1p1
            print("  [9] Testing: disk info nvme0n1p1")
            out_info = exec_cmd("disk info nvme0n1p1")
            print(f"      Info Output:\n{out_info.strip()}\n")
            assert "Device:           nvme0n1p1" in out_info
            assert "Type:             Partition (index 1)" in out_info
            assert "Size:             4M (4194304 bytes)" in out_info
            assert "Partition UUID:   11223344-5566-7788-99AA-BBCCDDEEFF00" in out_info
            assert "Partition Label:  Fortress Storage" in out_info
            assert "Mount point:      /mnt (ext2, read-write)" in out_info
            print("  [PASS] disk info verified.")

            # Test 9: disk info -c /dev/nvme0n1p1
            print("  [10] Testing: disk info -c /dev/nvme0n1p1")
            out_info_cmp = exec_cmd("disk info -c /dev/nvme0n1p1")
            print(f"      Info Comparison Output:\n{out_info_cmp.strip()}\n")
            assert "device=nvme0n1p1 type=partition index=1" in out_info_cmp
            assert "partuuid=11223344-5566-7788-99AA-BBCCDDEEFF00" in out_info_cmp
            assert "label=Fortress Storage" in out_info_cmp
            assert "mount=/mnt fs=ext2 flags=rw" in out_info_cmp
            print("  [PASS] disk info -c verified.")

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
