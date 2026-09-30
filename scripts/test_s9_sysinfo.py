#!/usr/bin/env python3
"""Phase 2: SYS_SYSINFO and /bin/sysinfo acceptance test.
Real shell, BIOS + UEFI, disposable ISO, no data disks, argv preflight.
Verifies RAM sanity, process count transition, and SMP=1 vs SMP=4 uptime rate.
"""
from pathlib import Path
import os
import re
import shutil
import socket
import subprocess
import tempfile
import threading
import time

REPO = Path(__file__).resolve().parent.parent
PROMPT_PATTERN = r"(?:fortress> |\[[a-zA-Z0-9_\-\./]+\]# )"


def qemu_command(mode, cpus, iso, variables, log, uart_path):
    cmd = [
        "qemu-system-x86_64", "-accel", "tcg", "-M", "q35", "-m", "2G",
        "-smp", str(cpus), "-display", "none", "-monitor", "none", "-no-reboot",
        "-boot", "d", "-cdrom", str(iso),
        "-chardev", f"socket,id=uart,path={uart_path},server=on,wait=off,logfile={log}",
        "-serial", "chardev:uart"
    ]
    if mode == "uefi":
        cmd += [
            "-drive", "if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
            "-drive", f"if=pflash,format=raw,unit=1,file={variables}"
        ]
    return cmd


def preflight(cmd, mode, cpus, iso, variables, log, uart_path):
    assert mode in ("bios", "uefi") and cpus in (1, 4)
    expected = qemu_command(mode, cpus, iso, variables, log, uart_path)
    assert cmd == expected, f"unexpected QEMU argv: {cmd} != {expected}"
    for token in ("-device", "nvme", "-hda", "-hdb", "-blockdev", "-snapshot"):
        assert token not in cmd, f"preflight found forbidden token {token}"


def parse_sysinfo(output_text):
    info = {}
    cpus_m = re.search(r"CPUs:\s+(\d+)", output_text)
    uptime_m = re.search(r"Uptime:\s+(\d+):(\d+):(\d+)", output_text)
    total_m = re.search(r"RAM total:\s+(\d+)\s+MiB", output_text)
    free_m = re.search(r"RAM free:\s+(\d+)\s+MiB", output_text)
    used_m = re.search(r"RAM used:\s+(\d+)\s+MiB", output_text)
    procs_m = re.search(r"Processes:\s+(\d+)", output_text)

    if cpus_m: info["cpus"] = int(cpus_m.group(1))
    if uptime_m:
        h, m, s = int(uptime_m.group(1)), int(uptime_m.group(2)), int(uptime_m.group(3))
        info["uptime_sec"] = h * 3600 + m * 60 + s
    if total_m: info["total_mib"] = int(total_m.group(1))
    if free_m: info["free_mib"] = int(free_m.group(1))
    if used_m: info["used_mib"] = int(used_m.group(1))
    if procs_m: info["procs"] = int(procs_m.group(1))
    return info


def run_session(mode, cpus, iso, tmp):
    log = REPO / "build" / f"s9-sysinfo-{mode}-{cpus}.log"
    log.write_text("")
    uart_path = tmp / f"uart-{mode}-{cpus}"
    variables = tmp / f"vars-{mode}-{cpus}.fd"
    if mode == "uefi":
        shutil.copyfile("/usr/share/OVMF/OVMF_VARS_4M.fd", variables)

    cmd = qemu_command(mode, cpus, iso, variables, log, uart_path)
    preflight(cmd, mode, cpus, iso, variables, log, uart_path)

    # Reject injection attempts
    for extra in (["-drive", "file=unsafe.img"], ["-device", "nvme"], ["-hda", "unsafe.img"]):
        try:
            preflight(cmd + extra, mode, cpus, iso, variables, log, uart_path)
        except AssertionError:
            pass
        else:
            raise AssertionError("preflight accepted injected storage device")

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
                    assert proc.poll() is None, "QEMU terminated prematurely"
                    time.sleep(0.05)
                raise TimeoutError(f"Prompt wait timed out: {get_text()[-1000:]}")

            def exec_cmd(cmd_str):
                start = len(get_text())
                for b in cmd_str.encode() + b"\n":
                    uart.send(bytes([b]))
                    time.sleep(0.005)
                res = wait_for_prompt(start)
                return res

            # Wait for shell prompt
            wait_for_prompt(0, timeout=60)

            # First sysinfo read
            out1 = exec_cmd("/bin/sysinfo")
            info1 = parse_sysinfo(out1)
            assert "cpus" in info1 and info1["cpus"] == cpus, f"Expected {cpus} CPUs, got {info1}"
            assert "total_mib" in info1 and "free_mib" in info1 and "used_mib" in info1, f"Missing RAM info: {out1}"
            assert info1["total_mib"] >= info1["free_mib"], f"total < free: {info1}"
            assert info1["total_mib"] > 1800 and info1["total_mib"] <= 2048, f"Implausible total RAM: {info1['total_mib']}"
            assert abs((info1["free_mib"] + info1["used_mib"]) - info1["total_mib"]) <= 1, f"RAM math mismatch: {info1}"
            assert "procs" in info1 and info1["procs"] >= 1, f"Invalid task count: {info1}"

            # Wait 2 seconds wall clock
            time.sleep(2.0)

            # Second sysinfo read
            out2 = exec_cmd("/bin/sysinfo")
            info2 = parse_sysinfo(out2)
            assert "uptime_sec" in info2 and "uptime_sec" in info1
            delta_uptime = info2["uptime_sec"] - info1["uptime_sec"]
            assert delta_uptime >= 1, f"Uptime did not advance: {info1['uptime_sec']} -> {info2['uptime_sec']}"

            # Spawn a background job and assert task_count increases
            before_procs = info2["procs"]
            exec_cmd("cat &")
            out3 = exec_cmd("/bin/sysinfo")
            info3 = parse_sysinfo(out3)
            assert "procs" in info3
            assert info3["procs"] > before_procs, f"Task count did not increase: before={before_procs}, after={info3['procs']}"

            return {
                "mode": mode,
                "cpus": cpus,
                "delta_uptime": delta_uptime,
                "info1": info1,
                "info2": info2,
                "info3": info3
            }

        finally:
            stop_reader.set()
            if uart:
                uart.close()
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)


def main():
    print("=== Shell S9 Phase 2: SYS_SYSINFO and /bin/sysinfo Acceptance Suite ===")
    with tempfile.TemporaryDirectory(prefix="fortress-s9-sysinfo-") as directory:
        tmp = Path(directory)
        iso = tmp / "sysinfo.iso"
        shutil.copyfile(REPO / "bin/fortress.iso", iso)

        results = {}
        for mode in ("bios", "uefi"):
            for cpus in (1, 4):
                print(f"[*] Testing {mode.upper()} SMP={cpus}...", flush=True)
                res = run_session(mode, cpus, iso, tmp)
                results[(mode, cpus)] = res
                print(f"    PASS: CPUs={res['info1']['cpus']}, RAM={res['info1']['total_mib']} MiB "
                      f"(Free={res['info1']['free_mib']}, Used={res['info1']['used_mib']}), "
                      f"Uptime delta={res['delta_uptime']}s, Procs={res['info1']['procs']}->{res['info3']['procs']}",
                      flush=True)

        # Regression Guard: SMP=1 vs SMP=4 uptime advancement rate
        for mode in ("bios", "uefi"):
            d1 = results[(mode, 1)]["delta_uptime"]
            d4 = results[(mode, 4)]["delta_uptime"]
            print(f"[*] Regression Guard {mode.upper()}: SMP=1 delta={d1}s, SMP=4 delta={d4}s")
            # If SMP=4 suffered the multi-writer bug, d4 would be ~4x d1 (e.g. 8s vs 2s).
            # With BSP-only timer, both must be close (ratio < 2.5).
            assert d4 < d1 * 2.5 + 2, f"Regression detected! SMP=4 uptime advancing too fast: {d4}s vs SMP=1 {d1}s"
            print(f"    PASS: SMP=1 vs SMP=4 uptime rate verified (BSP timebase invariant held)")

    print("\nPASS test-s9-sysinfo: All tests passed on BIOS & UEFI (SMP=1 and SMP=4) with no data disks!")


if __name__ == "__main__":
    main()
