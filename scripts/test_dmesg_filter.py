#!/usr/bin/env python3
"""QEMU Integration Test for /bin/dmesg facility filtering (-f, --facility) across BIOS & UEFI."""
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
sys.path.insert(0, str(REPO / "scripts"))
from test_nmi_transitions import connect

PROMPT_PATTERN = r"(?:fortress>|(?:\[-?\d+\] )?fortress:[^\r\n]*\$|\[[a-zA-Z0-9_\-\./]+\]#|[A-Za-z0-9_-]+@[A-Za-z0-9-]+:[^\r\n]*[#$])\s*"
CODE = Path("/usr/share/OVMF/OVMF_CODE_4M.fd")
VARS = Path("/usr/share/OVMF/OVMF_VARS_4M.fd")
ISO = REPO / "bin" / "fortress.iso"
NVME_SRC = REPO / "build" / "nvme_gpt.img"


def run_dmesg_filter_qemu(firmware="bios", smp=1):
    print(f"\n=== Running dmesg -f QEMU Integration Test ({firmware.upper()}, SMP={smp}) ===")
    assert ISO.is_file(), f"Bootable ISO missing at {ISO}"
    assert NVME_SRC.is_file(), f"NVMe disk image missing at {NVME_SRC}"

    with tempfile.TemporaryDirectory(prefix=f"fortress-dmesg-filter-{firmware}-smp{smp}-") as tmp:
        tmp_dir = Path(tmp)
        uart_path = tmp_dir / "uart"
        log_path = tmp_dir / "serial.log"
        nvme_path = tmp_dir / "nvme.img"
        shutil.copyfile(NVME_SRC, nvme_path)

        qemu_cmd = [
            "qemu-system-x86_64", "-M", "q35", "-m", "2G", "-smp", str(smp), "-display", "none",
            "-no-reboot", "-boot", "d", "-cdrom", str(ISO),
            "-drive", f"file={nvme_path},if=none,id=nvm0,format=raw,snapshot=off",
            "-device", "nvme,serial=fortress0,drive=nvm0",
            "-fw_cfg", "name=opt/fortress/write_test,string=1",
            "-chardev", f"socket,id=uart,path={uart_path},server=on,wait=off,logfile={log_path}",
            "-serial", "chardev:uart"
        ]

        if firmware == "uefi":
            if not CODE.is_file() or not VARS.is_file():
                print("  [SKIP] Paired OVMF 4M firmware not available on host.")
                return
            vars_path = tmp_dir / "vars.fd"
            shutil.copyfile(VARS, vars_path)
            qemu_cmd += [
                "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={CODE}",
                "-drive", f"if=pflash,format=raw,unit=1,file={vars_path}"
            ]

        proc = subprocess.Popen(qemu_cmd, cwd=REPO)
        try:
            uart = connect(uart_path)
            stop_drain = threading.Event()

            def drain():
                uart.settimeout(0.2)
                while not stop_drain.is_set():
                    try:
                        if not uart.recv(4096):
                            return
                    except (socket.timeout, OSError):
                        continue

            threading.Thread(target=drain, daemon=True).start()

            def get_text():
                if not log_path.exists():
                    return ""
                raw = log_path.read_text(errors="replace")
                return re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "", raw).replace("\r", "")

            sent_login = False
            def wait_for_prompt(start_pos=0, timeout=45, require_newline=False):
                nonlocal sent_login
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    txt = get_text()
                    chunk = txt[start_pos:]
                    if not sent_login and "FortressOS login: " in chunk:
                        sent_login = True
                        uart.sendall(b"operator\n")
                    if require_newline:
                        nl = chunk.find("\n")
                        if nl != -1 and re.search(r"(?:" + PROMPT_PATTERN + r")\s*\Z", chunk[nl+1:]):
                            return txt
                    else:
                        if re.search(r"(?:" + PROMPT_PATTERN + r")\s*\Z", chunk):
                            return txt
                    assert proc.poll() is None, f"QEMU exited prematurely with code {proc.poll()}"
                    time.sleep(0.05)
                full = get_text()
                raise AssertionError(f"Timeout waiting for prompt after pos {start_pos}. Tail:\n{full[-1000:]}")

            def send_str(s):
                for b in s.encode():
                    uart.send(bytes([b]))
                    time.sleep(0.005)

            def exec_cmd(cmd_str, timeout=15):
                pos = len(get_text())
                send_str(cmd_str + "\n")
                wait_for_prompt(pos, timeout=timeout, require_newline=True)
                return get_text()[pos:]

            def get_command_output(cmd_str, timeout=15):
                raw = exec_cmd(cmd_str, timeout=timeout)
                lines = []
                for line in raw.splitlines():
                    l = line.strip()
                    if not l:
                        continue
                    if cmd_str in l:
                        continue
                    if re.search(PROMPT_PATTERN, l):
                        continue
                    if l.startswith("history:") or l.startswith("WARNING:"):
                        continue
                    lines.append(l)
                return lines

            print("  [1] Waiting for interactive shell prompt...")
            wait_for_prompt(0, timeout=45, require_newline=False)
            print("  [1] Shell prompt ready.")

            # Elevate to root shell via sudo /bin/shell so dmesg (both builtin and /bin/dmesg) has permission
            print("  [2] Elevating to root shell (sudo /bin/shell)...")
            pos_sudo = len(get_text())
            send_str("sudo /bin/shell\n")
            wait_for_prompt(pos_sudo, timeout=15, require_newline=True)
            print("  [2] Root shell ready.")

            # Ensure /mnt is writable
            exec_cmd("chmod 777 /mnt")
            exec_cmd("sync")

            # Test 1: dmesg --help
            print("  [3] Testing: dmesg --help")
            out_help = exec_cmd("dmesg --help")
            assert "-f FAC" in out_help or "--facility" in out_help, f"Missing -f in help: {out_help}"
            assert "NET" in out_help, f"Missing supported facility in help: {out_help}"
            print("  [PASS] dmesg --help verified.")

            # Test 2: dmesg -f NET — only [NET] lines
            print("  [4] Testing: dmesg -f NET")
            net_lines = get_command_output("dmesg -f NET")
            assert len(net_lines) > 0, f"Expected [NET] lines in dmesg -f NET, got nothing: {net_lines}"
            for line in net_lines:
                assert re.match(r"^\[NET\]", line, re.IGNORECASE), f"Non-NET line in dmesg -f NET: {line}"
            print(f"  [PASS] dmesg -f NET returned {len(net_lines)} lines, all starting with [NET].")

            # Test 3: dmesg -f NET -n 5 — 5 lines max
            print("  [5] Testing: dmesg -f NET -n 5")
            net5_lines = get_command_output("dmesg -f NET -n 5")
            assert 1 <= len(net5_lines) <= 5, f"Expected 1..5 lines, got {len(net5_lines)}: {net5_lines}"
            for line in net5_lines:
                assert re.match(r"^\[NET\]", line, re.IGNORECASE), f"Non-NET line in dmesg -f NET -n 5: {line}"
            print(f"  [PASS] dmesg -f NET -n 5 returned {len(net5_lines)} lines (<= 5).")

            # Test 4: dmesg -f NET -f USB — verify both facilities allowed
            print("  [6] Testing: dmesg -f NET -f USB")
            net_usb_lines = get_command_output("dmesg -f NET -f USB")
            assert len(net_usb_lines) > 0, f"Expected lines in dmesg -f NET -f USB, got nothing: {net_usb_lines}"
            for line in net_usb_lines:
                is_net = bool(re.match(r"^\[NET\]", line, re.IGNORECASE))
                is_usb = bool(re.match(r"^\[USB\]", line, re.IGNORECASE))
                assert is_net or is_usb, f"Line not matching NET or USB: {line}"
            print(f"  [PASS] dmesg -f NET -f USB verified (all {len(net_usb_lines)} lines are NET or USB).")

            # Test 4b: dmesg -f NET -f PCI — verify multiple present facilities appear
            print("  [6b] Testing: dmesg -f NET -f PCI")
            net_pci_lines = get_command_output("dmesg -f NET -f PCI")
            assert len(net_pci_lines) > 0, f"Expected lines in dmesg -f NET -f PCI, got nothing: {net_pci_lines}"
            has_net = False
            has_pci = False
            for line in net_pci_lines:
                is_net = bool(re.match(r"^\[NET\]", line, re.IGNORECASE))
                is_pci = bool(re.match(r"^\[PCI\]", line, re.IGNORECASE))
                assert is_net or is_pci, f"Line not matching NET or PCI: {line}"
                if is_net: has_net = True
                if is_pci: has_pci = True
            assert has_net and has_pci, f"Expected both NET and PCI: NET={has_net}, PCI={has_pci}"
            print(f"  [PASS] dmesg -f NET -f PCI verified (both NET and PCI appeared, total={len(net_pci_lines)} lines).")

            # Test 5: dmesg -f NONEXISTENT — verify empty, exit 0
            print("  [7] Testing: dmesg -f NONEXISTENT")
            none_lines = get_command_output("dmesg -f NONEXISTENT")
            assert len(none_lines) == 0, f"Expected empty output for NONEXISTENT, got: {none_lines}"
            status_lines = get_command_output("echo $?")
            assert any(l == "0" for l in status_lines), f"Expected exit status 0 for NONEXISTENT, got: {status_lines}"
            print("  [PASS] dmesg -f NONEXISTENT returned empty output and exit status 0.")

            # Test 6: dmesg -f NET /mnt/net.log — verify file contains only [NET] lines
            print("  [8] Testing: dmesg -f NET /mnt/net.log")
            exec_cmd("rm -f /mnt/net.log")
            exec_cmd("sync")
            save_lines = get_command_output("dmesg -f NET /mnt/net.log")
            assert any("Saved" in l and "/mnt/net.log" in l for l in save_lines), f"Expected Saved output, got: {save_lines}"
            exec_cmd("sync")
            file_lines = get_command_output("cat /mnt/net.log")
            assert len(file_lines) > 0, f"Expected non-empty /mnt/net.log, got: {file_lines}"
            for line in file_lines:
                assert re.match(r"^\[NET\]", line, re.IGNORECASE), f"Non-NET line in saved file: {line}"
            print(f"  [PASS] /mnt/net.log contains {len(file_lines)} lines, all starting with [NET].")

            # Test 7: Standalone binary /bin/dmesg -f NET -n 3
            print("  [9] Testing standalone /bin/dmesg -f NET -n 3")
            bin_lines = get_command_output("/bin/dmesg -f NET -n 3")
            assert 1 <= len(bin_lines) <= 3, f"Expected 1..3 lines from /bin/dmesg, got {len(bin_lines)}: {bin_lines}"
            for line in bin_lines:
                assert re.match(r"^\[NET\]", line, re.IGNORECASE), f"Non-NET line in /bin/dmesg: {line}"
            print(f"  [PASS] Standalone /bin/dmesg -f NET -n 3 verified ({len(bin_lines)} lines).")

            # Test 8: Unfiltered dmesg regression check
            print("  [10] Testing regression: dmesg -n 10 (unfiltered)")
            unf_lines = get_command_output("dmesg -n 10")
            assert len(unf_lines) > 0, f"Expected lines from unfiltered dmesg: {unf_lines}"
            print(f"  [PASS] Unfiltered dmesg returned {len(unf_lines)} lines.")

            # Test 9: dmesg -l err — returns exactly level 3 log
            print("  [11] Testing: dmesg -l err")
            err_lines = get_command_output("dmesg -l err")
            assert len(err_lines) == 1, f"Expected exactly 1 line for dmesg -l err, got {len(err_lines)}: {err_lines}"
            assert "test log level 3 (err)" in err_lines[0], f"Expected level 3 err log, got: {err_lines[0]}"
            print(f"  [PASS] dmesg -l err returned exactly level 3 log: {err_lines[0]}")

            # Test 10: dmesg -l err,crit — returns levels 2 and 3
            print("  [12] Testing: dmesg -l err,crit")
            err_crit_lines = get_command_output("dmesg -l err,crit")
            assert len(err_crit_lines) == 2, f"Expected exactly 2 lines for dmesg -l err,crit, got {len(err_crit_lines)}: {err_crit_lines}"
            assert any("level 2 (crit)" in l for l in err_crit_lines), f"Missing level 2 in {err_crit_lines}"
            assert any("level 3 (err)" in l for l in err_crit_lines), f"Missing level 3 in {err_crit_lines}"
            print(f"  [PASS] dmesg -l err,crit returned exactly levels 2 and 3: {err_crit_lines}")

            # Test 11: dmesg -f TEST -l warn — correctly intersects facility and level
            print("  [13] Testing: dmesg -f TEST -l warn")
            test_warn_lines = get_command_output("dmesg -f TEST -l warn")
            assert len(test_warn_lines) == 1, f"Expected exactly 1 line for dmesg -f TEST -l warn, got {len(test_warn_lines)}: {test_warn_lines}"
            assert "test log level 4 (warn)" in test_warn_lines[0], f"Expected level 4 warn log, got: {test_warn_lines[0]}"
            print(f"  [PASS] dmesg -f TEST -l warn correctly intersected: {test_warn_lines[0]}")

            # Test 12: dmesg -l 3 (numeric)
            print("  [14] Testing: dmesg -l 3")
            num_lines = get_command_output("dmesg -l 3")
            assert len(num_lines) == 1 and "level 3 (err)" in num_lines[0], f"Expected level 3 for numeric, got: {num_lines}"
            print("  [PASS] dmesg -l 3 numeric filtering verified.")

            # Test 13: dmesg -l invalid — exit code 2
            print("  [15] Testing: dmesg -l invalid")
            exec_cmd("dmesg -l invalid")
            status_lines = get_command_output("echo $?")
            assert any(l == "2" for l in status_lines), f"Expected exit status 2 for invalid level, got: {status_lines}"
            print("  [PASS] dmesg -l invalid rejected with exit status 2.")

            # Power off
            print("  [16] Powering off...")
            send_str("poweroff\n")
            proc.wait(timeout=10)
            stop_drain.set()
            print(f">>> {firmware.upper()} SMP={smp} dmesg -f / -l INTEGRATION TEST PASSED <<<")

        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()


def main():
    print("==========================================================")
    print(" FortressOS dmesg Facility Filter Multi-Firmware Suite    ")
    print("==========================================================")
    run_dmesg_filter_qemu(firmware="bios", smp=1)
    run_dmesg_filter_qemu(firmware="uefi", smp=1)
    print("\n>>> ALL dmesg -f QEMU INTEGRATION TESTS PASSED (100%) <<<")


if __name__ == "__main__":
    main()
