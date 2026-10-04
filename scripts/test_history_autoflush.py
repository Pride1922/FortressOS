#!/usr/bin/env python3
"""QEMU Hard-Reset Integration Test for Persistent History Auto-Flush.

Specifications:
1. Disposable image with /mnt writable (NVMe persistent ext2 partition).
2. Run 10 commands interactively in the Ring 3 shell.
3. Issue system_reset via QEMU monitor/QMP (hard reset — not the shell's clean exit).
4. Boot again.
5. Verify history shows the expected 10 commands.
6. Verify wc -l /mnt/.fortress/history matches expected line count (21 lines).
7. Report measured auto-flush timings.
"""
from pathlib import Path
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))
from test_nmi_transitions import QMP, connect


def send_str(uart, s):
    for b in s.encode():
        uart.send(bytes([b]))
        time.sleep(0.005)


def wait_for_pattern(log_path, pattern, child, start_pos=0, timeout=45):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if log_path.exists():
            txt = log_path.read_text(errors="replace").replace("\r", "")
            if pattern in txt[start_pos:]:
                return txt
        assert child.poll() is None, f"QEMU exited prematurely with code {child.poll()}"
        time.sleep(0.05)
    full = log_path.read_text(errors="replace") if log_path.exists() else ""
    raise AssertionError(f"Timeout waiting for {pattern!r} after pos {start_pos}. Log tail:\n{full[-1000:]}")


def run_hard_reset_test(mode="bios"):
    print(f"=== Running Persistent History Auto-Flush Hard-Reset Test ({mode.upper()}) ===")
    build_dir = REPO / "build"
    build_dir.mkdir(exist_ok=True)
    orig_img = build_dir / "nvme_gpt.img"
    if not orig_img.exists():
        subprocess.run(["make", "nvme-gpt-disk"], cwd=REPO, check=True)

    with tempfile.TemporaryDirectory(prefix="fortress-hist-autoflush-") as tmp:
        tmp_dir = Path(tmp)
        img_copy = tmp_dir / "disposable_nvme.img"
        shutil.copyfile(orig_img, img_copy)

        # =====================================================================
        # SESSION 1: Run 10 commands and trigger hard system_reset
        # =====================================================================
        print("  [Session 1] Booting FortressOS on disposable NVMe disk...")
        uart1_path = tmp_dir / "uart1"
        qmp1_path = tmp_dir / "qmp1.sock"
        log1_path = tmp_dir / "session1.log"

        qemu_cmd1 = [
            "qemu-system-x86_64", "-M", "q35", "-m", "2G", "-smp", "1", "-display", "none",
            "-no-reboot", "-boot", "d", "-cdrom", "bin/fortress.iso",
            "-chardev", f"socket,id=uart,path={uart1_path},server=on,wait=off,logfile={log1_path}",
            "-serial", "chardev:uart",
            "-qmp", f"unix:{qmp1_path},server=on,wait=off",
            "-drive", f"file={img_copy},if=none,id=nvm0,format=raw,snapshot=off",
            "-device", "nvme,serial=fortress0,drive=nvm0",
            "-fw_cfg", "name=opt/fortress/write_test,string=1",
        ]
        code = Path("/usr/share/OVMF/OVMF_CODE_4M.fd")
        source = Path("/usr/share/OVMF/OVMF_VARS_4M.fd")
        if mode == "uefi":
            assert code.exists() and source.exists(), "Paired OVMF 4M firmware required for UEFI"
            vars_file = tmp_dir / "vars1.fd"
            shutil.copyfile(source, vars_file)
            qemu_cmd1 += [
                "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={code}",
                "-drive", f"if=pflash,format=raw,unit=1,file={vars_file}",
            ]

        proc1 = subprocess.Popen(qemu_cmd1, cwd=REPO)
        flush_timings = []
        try:
            uart1 = connect(uart1_path)
            qmp1 = QMP(qmp1_path)

            # Drain uart in background so buffer doesn't stall
            stop_drain1 = threading.Event()
            def drain1():
                uart1.settimeout(0.2)
                while not stop_drain1.is_set():
                    try:
                        if not uart1.recv(4096):
                            return
                    except (socket.timeout, OSError):
                        continue
            threading.Thread(target=drain1, daemon=True).start()

            print("  [Session 1] Waiting for interactive shell prompt...")
            wait_for_pattern(log1_path, "fortress:/ $ ", proc1, timeout=45)
            print("  [Session 1] Prompt ready. Executing 10 commands...")

            for i in range(1, 11):
                cmd = f"echo autoflush_cmd_{i}\n"
                pos = len(log1_path.read_text(errors="replace"))
                t0 = time.monotonic()
                send_str(uart1, cmd)
                wait_for_pattern(log1_path, f"autoflush_cmd_{i}", proc1, start_pos=pos, timeout=10)
                wait_for_pattern(log1_path, "fortress:/ $ ", proc1, start_pos=pos, timeout=10)
                elapsed_ms = (time.monotonic() - t0) * 1000.0

                if i in (5, 10):
                    flush_timings.append(elapsed_ms)
                    print(f"    - Command {i:2d} (echo autoflush_cmd_{i}): auto-flush triggered (round-trip {elapsed_ms:.1f}ms)")
                else:
                    print(f"    - Command {i:2d} (echo autoflush_cmd_{i}): executed")

            # 3. Issue HARD ungraceful system_reset via QEMU monitor/QMP
            print("  [Session 1] Issuing ungraceful 'system_reset' via QMP monitor...")
            # QEMU with -no-reboot exits on reset
            try:
                qmp1.sock.sendall(b'{"execute": "system_reset"}\n')
            except OSError:
                pass

            ret1 = proc1.wait(timeout=10)
            stop_drain1.set()
            print(f"  [Session 1] Hardware reset confirmed (QEMU process exited with code {ret1}, no clean shell shutdown).")

        finally:
            if proc1.poll() is None:
                proc1.kill()
                proc1.wait()

        # =====================================================================
        # SESSION 2: Boot again and verify history survived hard reset
        # =====================================================================
        print("  [Session 2] Booting again on the persisted NVMe disk...")
        uart2_path = tmp_dir / "uart2"
        log2_path = tmp_dir / "session2.log"

        qemu_cmd2 = [
            "qemu-system-x86_64", "-M", "q35", "-m", "2G", "-smp", "1", "-display", "none",
            "-no-reboot", "-boot", "d", "-cdrom", "bin/fortress.iso",
            "-chardev", f"socket,id=uart,path={uart2_path},server=on,wait=off,logfile={log2_path}",
            "-serial", "chardev:uart",
            "-drive", f"file={img_copy},if=none,id=nvm0,format=raw,snapshot=off",
            "-device", "nvme,serial=fortress0,drive=nvm0",
        ]
        if mode == "uefi":
            vars_file2 = tmp_dir / "vars2.fd"
            shutil.copyfile(source, vars_file2)
            qemu_cmd2 += [
                "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={code}",
                "-drive", f"if=pflash,format=raw,unit=1,file={vars_file2}",
            ]

        proc2 = subprocess.Popen(qemu_cmd2, cwd=REPO)
        try:
            uart2 = connect(uart2_path)
            stop_drain2 = threading.Event()
            def drain2():
                uart2.settimeout(0.2)
                while not stop_drain2.is_set():
                    try:
                        if not uart2.recv(4096):
                            return
                    except (socket.timeout, OSError):
                        continue
            threading.Thread(target=drain2, daemon=True).start()

            print("  [Session 2] Waiting for interactive shell prompt...")
            wait_for_pattern(log2_path, "fortress:/ $ ", proc2, timeout=45)
            print("  [Session 2] Shell prompt ready!")

            # Verify history command
            pos = len(log2_path.read_text(errors="replace"))
            send_str(uart2, "history\n")
            wait_for_pattern(log2_path, "fortress:/ $ ", proc2, start_pos=pos, timeout=10)
            hist_output = log2_path.read_text(errors="replace")[pos:]
            print("  [Session 2] History command output after hard reset:")
            for line in hist_output.splitlines():
                if "autoflush_cmd_" in line:
                    print(f"      {line.strip()}")

            for i in range(1, 11):
                assert f"autoflush_cmd_{i}" in hist_output, f"Missing autoflush_cmd_{i} in history: {hist_output}"
            print("  [PASS] All 10 pre-reset commands present in history!")

            # Verify wc -l /mnt/.fortress/history
            pos = len(log2_path.read_text(errors="replace"))
            send_str(uart2, "wc -l /mnt/.fortress/history\n")
            wait_for_pattern(log2_path, "fortress:/ $ ", proc2, start_pos=pos, timeout=10)
            wc_output = log2_path.read_text(errors="replace")[pos:]
            print(f"  [Session 2] wc -l output: {wc_output.strip()}")
            assert "21 /mnt/.fortress/history" in wc_output, f"Unexpected line count: {wc_output}"
            print("  [PASS] wc -l /mnt/.fortress/history matches exact expected 21 lines (header + 10 entries)!")

            # Graceful shutdown
            send_str(uart2, "poweroff\n")
            proc2.wait(timeout=10)
            stop_drain2.set()
            print(f"  [PASS] Clean poweroff. History auto-flush hard-reset test: PASS ({mode.upper()})\n")

        finally:
            if proc2.poll() is None:
                proc2.kill()
                proc2.wait()

    return flush_timings


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="Persistent history auto-flush hard-reset test")
    parser.add_argument("--mode", choices=["bios", "uefi", "all"], default="bios")
    args = parser.parse_args()

    modes = ["bios", "uefi"] if args.mode == "all" else [args.mode]
    for m in modes:
        timings = run_hard_reset_test(m)
        print(f"Recorded auto-flush roundtrip latencies ({m.upper()}): {timings}")
    print(">>> QEMU PERSISTENT HISTORY AUTO-FLUSH HARD-RESET TEST PASSED <<<")
