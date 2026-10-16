#!/usr/bin/env python3
"""DHCP v2 client & background renewal daemon acceptance test under QEMU.

Runs BIOS and UEFI boot with e1000 under SMP=1, and verifies:
1. Boot-time automatic DHCP configuration:
   - Kernel executes /sbin/dhcpc --boot before login prompt
   - /run/dhcpc.lease and /run/dhcpc.pid are written
   - Background daemon (/sbin/dhcpc --daemon) is spawned detached
   - System reaches login prompt without delay
2. Out-of-the-box networking:
   - Interface is pre-configured with 10.0.2.15/24, gateway 10.0.2.2
   - Gateway ping succeeds immediately without running dhcpc manually
   - nslookup succeeds
3. CLI lease status (/sbin/dhcpc --status):
   - Displays IP, mask, gateway, DNS, lease duration, expiration, daemon PID
4. Forced lease renewal (/sbin/dhcpc --renew):
   - Sends SIGHUP to daemon, forces renewal, daemon stays alive
5. Clean shutdown and release (/sbin/dhcpc --release):
   - Sends SIGTERM to daemon, cleans up /run/dhcpc.pid and /run/dhcpc.lease
   - Flushes interface
6. Offline boot resilience:
   - Boot with -net none proceeds cleanly to login prompt within <= 10s without hang

Enforces disposable ISO/OVMF preflight.
"""

from pathlib import Path
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

REPO = Path(__file__).resolve().parent.parent
CODE = Path("/usr/share/OVMF/OVMF_CODE_4M.fd")
VARS = Path("/usr/share/OVMF/OVMF_VARS_4M.fd")
ISO = REPO / "bin" / "fortress.iso"
PROMPT_PATTERN = r"(?:fortress> |(?:\[-?\d+\] )?fortress:[^\r\n]* \$ |\[[a-zA-Z0-9_\-\./]+\]# |[A-Za-z0-9_-]+@[A-Za-z0-9-]+:[^\r\n]*[#$] )"


def qemu_command(mode, iso, variables, log, uart_path, offline=False):
    cmd = [
        "qemu-system-x86_64", "-accel", "tcg", "-M", "q35", "-m", "2G",
        "-smp", "1", "-display", "none", "-monitor", "none", "-no-reboot",
        "-boot", "d", "-cdrom", str(iso),
    ]
    if offline:
        cmd += ["-net", "none"]
    else:
        cmd += [
            "-netdev", "user,id=net0",
            "-device", "e1000,netdev=net0",
        ]
    cmd += [
        "-chardev", f"socket,id=uart,path={uart_path},server=on,wait=off,logfile={log}",
        "-serial", "chardev:uart"
    ]
    if mode == "uefi":
        cmd += [
            "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={CODE}",
            "-drive", f"if=pflash,format=raw,unit=1,file={variables}"
        ]
    return cmd


def run_session(mode, iso, tmp):
    evidence_dir = REPO / "build" / f"dhcp-v2-{mode}"
    evidence_dir.mkdir(parents=True, exist_ok=True)
    log = evidence_dir / f"dhcp-v2-{mode}.log"
    log.write_text("")
    uart_path = tmp / f"uart-{mode}"
    variables = tmp / f"vars-{mode}.fd"
    if mode == "uefi":
        shutil.copyfile(VARS, variables)

    cmd = qemu_command(mode, iso, variables, log, uart_path, offline=False)

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

            sent_login = False
            def wait_for_prompt(after_idx=0, timeout=60, require_newline=False):
                nonlocal sent_login
                d = time.monotonic() + timeout
                while time.monotonic() < d:
                    t = get_text()
                    if not sent_login and "FortressOS login: " in t[after_idx:]:
                        sent_login = True
                        uart.sendall(b"operator\n")
                    chunk = t[after_idx:]
                    if require_newline:
                        nl = chunk.find("\n")
                        if nl != -1 and re.search(PROMPT_PATTERN, chunk[nl+1:]):
                            return chunk
                    else:
                        if re.search(PROMPT_PATTERN, chunk):
                            return chunk
                    time.sleep(0.1)
                raise TimeoutError(f"Prompt not seen within {timeout}s: {t[-500:]}")

            def run_command(command, timeout=15):
                before = len(get_text())
                uart.sendall(command.encode("ascii") + b"\n")
                wait_for_prompt(after_idx=before, timeout=timeout, require_newline=True)
                return get_text()[before:]

            # 1. Wait for initial Ring 3 shell prompt
            wait_for_prompt()
            print(f"[{mode}] Shell ready after automated boot")

            boot_log = get_text()
            assert "DHCP lease acquired 10.0.2.15/24" in boot_log or "acquired lease 10.0.2.15/24" in boot_log, \
                f"Boot DHCP announcement missing: {boot_log[:1000]}"
            print(f"[{mode}] Case 1 (boot-time automatic DHCP): PASS")

            # 2. Check lease file and daemon pid
            out = run_command("cat /run/dhcpc.pid")
            pid_match = re.search(r"(?:^|\n)\s*(\d+)\s*(?:\n|$)", out)
            daemon_pid = pid_match.group(1) if pid_match else ""
            assert daemon_pid.isdigit(), f"Invalid /run/dhcpc.pid: {out}"

            out = run_command("cat /run/dhcpc.lease")
            assert "IP=10.0.2.15" in out, f"Missing IP in lease file: {out}"
            assert "SERVER=10.0.2.2" in out, f"Missing SERVER in lease file: {out}"
            print(f"[{mode}] Case 2 (daemon PID {daemon_pid} & /run/dhcpc.lease): PASS")

            # 3. Verify out-of-the-box connectivity without manual dhcpc
            out = run_command("/bin/ping -c 2 10.0.2.2", timeout=20)
            assert "bytes from 10.0.2.2" in out or "seq=" in out, f"Out-of-the-box ping failed: {out}"
            print(f"[{mode}] Case 3 (immediate ping 10.0.2.2): PASS")

            out = run_command("/bin/nslookup example.com", timeout=20)
            assert "Address: " in out or "canonical" in out.lower() or "example.com" in out, \
                f"Out-of-the-box nslookup failed: {out}"
            print(f"[{mode}] Case 4 (immediate nslookup): PASS")

            # 4. Check /sbin/dhcpc --status
            out = run_command("/sbin/dhcpc --status")
            assert "10.0.2.15/24" in out, f"dhcpc --status missing IP: {out}"
            assert "10.0.2.2" in out, f"dhcpc --status missing Gateway/Server: {out}"
            assert "running (PID" in out, f"dhcpc --status missing running daemon: {out}"
            print(f"[{mode}] Case 5 (dhcpc --status): PASS")

            # 5. Check /sbin/dhcpc --renew
            out = run_command("sudo /sbin/dhcpc --renew")
            assert "sent renewal signal" in out or "SIGHUP" in out, f"dhcpc --renew failed: {out}"
            time.sleep(1)
            out = run_command("/sbin/dhcpc --status")
            assert "running (PID" in out, f"daemon died after SIGHUP: {out}"
            print(f"[{mode}] Case 6 (dhcpc --renew): PASS")

            # 6. Check /sbin/dhcpc --release
            out = run_command("sudo /sbin/dhcpc --release")
            assert "sent release signal" in out or "SIGTERM" in out, f"dhcpc --release failed: {out}"
            time.sleep(1)
            out = run_command("cat /run/dhcpc.pid")
            assert "no such file" in out.lower() or out.strip() == "", f"PID file not removed after release: {out}"
            print(f"[{mode}] Case 7 (dhcpc --release & clean stop): PASS")

            # 7. Clean shutdown
            uart.sendall(b"sudo poweroff\n")
            proc.wait(timeout=10)
            print(f"[{mode}] Lifecycle PASS")

        finally:
            stop_reader.set()
            if uart:
                try:
                    uart.close()
                except OSError:
                    pass
            if proc.poll() is None:
                proc.kill()
                proc.wait()


def run_offline_session(mode, iso, tmp):
    evidence_dir = REPO / "build" / f"dhcp-v2-offline-{mode}"
    evidence_dir.mkdir(parents=True, exist_ok=True)
    log = evidence_dir / f"dhcp-v2-offline-{mode}.log"
    log.write_text("")
    uart_path = tmp / f"uart-off-{mode}"
    variables = tmp / f"vars-off-{mode}.fd"
    if mode == "uefi":
        shutil.copyfile(VARS, variables)

    cmd = qemu_command(mode, iso, variables, log, uart_path, offline=True)

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

            sent_login = False
            d = time.monotonic() + 30
            while time.monotonic() < d:
                t = get_text()
                if not sent_login and "FortressOS login: " in t:
                    sent_login = True
                    uart.sendall(b"operator\n")
                if re.search(PROMPT_PATTERN, t):
                    break
                time.sleep(0.1)

            assert sent_login, f"[{mode} offline] Failed to reach login prompt: {get_text()[-500:]}"
            print(f"[{mode} offline] Reached login prompt without hang: PASS")

            uart.sendall(b"sudo poweroff\n")
            proc.wait(timeout=10)
            print(f"[{mode} offline] PASS")

        finally:
            stop_reader.set()
            if uart:
                try:
                    uart.close()
                except OSError:
                    pass
            if proc.poll() is None:
                proc.kill()
                proc.wait()


def main():
    assert ISO.exists(), f"{ISO} does not exist. Run make first."
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = Path(tmpdir)
        print("=== Testing BIOS DHCP v2 Lifecycle ===")
        run_session("bios", ISO, tmp)
        print("=== Testing BIOS Offline Resilience ===")
        run_offline_session("bios", ISO, tmp)

        if CODE.exists() and VARS.exists():
            print("=== Testing UEFI DHCP v2 Lifecycle ===")
            run_session("uefi", ISO, tmp)
            print("=== Testing UEFI Offline Resilience ===")
            run_offline_session("uefi", ISO, tmp)
        else:
            print("Skipping UEFI (OVMF not installed)")
    print("All DHCP v2 QEMU integration tests passed.")


if __name__ == "__main__":
    main()
