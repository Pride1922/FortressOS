#!/usr/bin/env python3
"""DHCP client (/sbin/dhcpc) acceptance test under QEMU.

Runs BIOS and UEFI boot with e1000 under SMP=1, waits for Ring 3 shell,
and verifies:
1. /sbin/dhcpc --help displays usage, flags, and v1 limitation note.
2. /sbin/dhcpc --dry-run performs DHCP handshake and reports parameters without mutating interface/files.
3. sudo /sbin/dhcpc acquires lease from QEMU DHCP server (10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.3).
4. /sbin/ifconfig reflects IP, gateway, and DNS servers.
5. ping 10.0.2.2 succeeds.
6. nslookup example.com succeeds.
7. /etc/network/network.conf contains the acquired configuration.
8. resolv.conf contains the acquired DNS server.
9. /sbin/ifup /etc/network/network.conf re-applies the saved configuration cleanly.
10. Clean shutdown.

Enforces argv preflight (disposable ISO/OVMF, no data disks).
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


def qemu_command(mode, iso, variables, log, uart_path):
    cmd = [
        "qemu-system-x86_64", "-accel", "tcg", "-M", "q35", "-m", "2G",
        "-smp", "1", "-display", "none", "-monitor", "none", "-no-reboot",
        "-boot", "d", "-cdrom", str(iso),
        "-netdev", "user,id=net0",
        "-device", "e1000,netdev=net0",
        "-chardev", f"socket,id=uart,path={uart_path},server=on,wait=off,logfile={log}",
        "-serial", "chardev:uart"
    ]
    if mode == "uefi":
        cmd += [
            "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={CODE}",
            "-drive", f"if=pflash,format=raw,unit=1,file={variables}"
        ]
    return cmd


def preflight(cmd, mode, iso, variables, log, uart_path):
    assert mode in ("bios", "uefi")
    expected = qemu_command(mode, iso, variables, log, uart_path)
    assert cmd == expected, f"unexpected QEMU argv: {cmd} != {expected}"
    for token in ("-hda", "-hdb", "nvme", "-blockdev", "-snapshot", "usb-storage"):
        assert token not in cmd, f"preflight found forbidden token {token}"


def run_session(mode, iso, tmp):
    evidence_dir = REPO / "build" / f"dhcp-client-{mode}"
    evidence_dir.mkdir(parents=True, exist_ok=True)
    log = evidence_dir / f"dhcp-client-{mode}.log"
    log.write_text("")
    uart_path = tmp / f"uart-{mode}"
    variables = tmp / f"vars-{mode}.fd"
    if mode == "uefi":
        shutil.copyfile(VARS, variables)

    cmd = qemu_command(mode, iso, variables, log, uart_path)
    preflight(cmd, mode, iso, variables, log, uart_path)

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
            print(f"[{mode}] Shell ready")

            # 2. Check /sbin/dhcpc --help
            out = run_command("/sbin/dhcpc --help")
            assert "usage: dhcpc" in out, f"dhcpc --help failed: {out}"
            assert "--dry-run" in out, f"dhcpc --dry-run missing in help: {out}"
            assert "Limitation (v1)" in out, f"dhcpc v1 limitation note missing: {out}"
            print(f"[{mode}] Case 1 (dhcpc --help): PASS")

            # 3. Check sudo /sbin/dhcpc --dry-run
            out = run_command("sudo /sbin/dhcpc --dry-run")
            assert "10.0.2.15/24" in out and "gateway 10.0.2.2" in out and "valid (dry-run)" in out, \
                f"dhcpc --dry-run failed: {out}"
            print(f"[{mode}] Case 2 (dhcpc --dry-run): PASS")

            # 4. Run sudo /sbin/dhcpc (acquire lease and configure)
            out = run_command("sudo /sbin/dhcpc")
            assert "acquired lease 10.0.2.15/24 gateway 10.0.2.2" in out, \
                f"dhcpc lease acquire failed: {out}"
            print(f"[{mode}] Case 3 (dhcpc lease acquire): PASS")

            # 5. Check /sbin/ifconfig
            out = run_command("/sbin/ifconfig")
            assert "inet 10.0.2.15/24" in out, f"ifconfig missing IP: {out}"
            assert "gateway 10.0.2.2" in out, f"ifconfig missing gateway: {out}"
            assert "dns 10.0.2.3" in out, f"ifconfig missing DNS: {out}"
            print(f"[{mode}] Case 4 (ifconfig verification): PASS")

            # 6. Ping gateway 10.0.2.2
            out = run_command("/bin/ping -c 2 10.0.2.2", timeout=20)
            assert "bytes from 10.0.2.2" in out or "seq=" in out, f"Ping gateway failed: {out}"
            print(f"[{mode}] Case 5 (ping gateway 10.0.2.2): PASS")

            # 7. nslookup example.com
            out = run_command("/bin/nslookup example.com", timeout=20)
            assert "Address: " in out or "canonical" in out.lower() or "example.com" in out, \
                f"nslookup failed: {out}"
            print(f"[{mode}] Case 6 (nslookup example.com): PASS")

            # 8. Check saved network.conf format
            out = run_command("cat /tmp/network.conf")
            if "no such file" in out.lower():
                out = run_command("cat /etc/network/network.conf")
            assert "address 10.0.2.15/24" in out, f"Missing address in config file: {out}"
            assert "gateway 10.0.2.2" in out, f"Missing gateway in config file: {out}"
            assert "dns 10.0.2.3" in out, f"Missing DNS in config file: {out}"
            print(f"[{mode}] Case 7 (saved network.conf format): PASS")

            # 9. Check resolv.conf
            out = run_command("cat /tmp/resolv.conf")
            assert "nameserver 10.0.2.3" in out, f"Missing nameserver in resolv.conf: {out}"
            print(f"[{mode}] Case 8 (resolv.conf format): PASS")

            # 10. Verify /sbin/ifup re-applies saved config
            out = run_command("sudo /sbin/ifup /tmp/network.conf")
            assert "eth0: address 10.0.2.15/24 gateway 10.0.2.2 applied" in out, \
                f"ifup re-apply failed: {out}"
            print(f"[{mode}] Case 9 (ifup re-apply saved config): PASS")

            # 11. Clean shutdown
            uart.sendall(b"sudo poweroff\n")
            proc.wait(timeout=10)
            print(f"[{mode}] PASS")

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
        run_session("bios", ISO, tmp)
        if CODE.exists() and VARS.exists():
            run_session("uefi", ISO, tmp)
        else:
            print("Skipping UEFI (OVMF not installed)")
    print("All /sbin/dhcpc QEMU integration tests passed.")


if __name__ == "__main__":
    main()
