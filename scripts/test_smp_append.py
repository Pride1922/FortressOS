#!/usr/bin/env python3
"""SMP ext2 Concurrent Append Verification Runner (Phase 3 / Finding 5).

Verifies true multi-core concurrent append serialization under QEMU SMP:
1. Multi-firmware testing: legacy BIOS and UEFI with paired OVMF 4M firmware.
2. Runs under QEMU with multi-core SMP (-smp 4) and writable NVMe ext2 mount.
3. Scenario 1 (Independent handles): Two pinned worker threads open independent
   file handles with VFS_O_WRONLY | VFS_O_APPEND, release an atomic start barrier,
   and slam concurrent appends.
4. Scenario 2 (Shared handle): Two pinned worker threads share a single file_t
   handle with VFS_O_APPEND (atomic refcount incremented), release start barrier,
   and slam concurrent appends.
5. In-kernel verification:
   - File size exactly 3200 bytes (200 records * 16 bytes).
   - 100% of records delivered intact with 0 write errors.
   - 0 duplicate records, 0 corrupted records.
   - Strict monotonic per-worker record sequence preserved.
   - Interleaving transitions between workers observed and logged.
6. Offline filesystem audit:
   - Clean ACPI S5 shutdown via poweroff command.
   - Host e2fsck -fn reports 0 filesystem errors on the persisted partition.
"""
from pathlib import Path
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

CODE = Path("/usr/share/OVMF/OVMF_CODE_4M.fd")
VARS = Path("/usr/share/OVMF/OVMF_VARS_4M.fd")


def check_e2fsck(img_path):
    with tempfile.TemporaryDirectory(prefix="fortress-e2fsck-") as tmp:
        part_file = Path(tmp) / "part1.ext2"
        # Partition 1 starts at sector 2048 (1 MiB) with 8192 sectors (4 MiB)
        part_offset = 2048 * 512
        part_size = 8192 * 512
        with open(img_path, "rb") as f:
            f.seek(part_offset)
            data = f.read(part_size)
            assert len(data) == part_size, "Truncated partition data"
            part_file.write_bytes(data)

        res = subprocess.run(["e2fsck", "-fn", str(part_file)], capture_output=True, text=True)
        print("      - e2fsck output:\n" + "\n".join("        " + l for l in res.stdout.strip().splitlines()))
        assert res.returncode == 0, f"e2fsck reported filesystem errors (code {res.returncode}):\n{res.stdout}\n{res.stderr}"


def run_smp_append_test(mode="bios", cpus=4):
    orig_img = REPO / "build" / "nvme_gpt.img"
    if not orig_img.is_file():
        raise FileNotFoundError(f"Missing base fixture: {orig_img}")

    log_path = REPO / "build" / f"smp_append_{mode}_{cpus}.log"
    if log_path.exists():
        log_path.unlink()

    test_img = REPO / "build" / f"smp_append_fixture_{mode}_{cpus}.img"
    shutil.copyfile(orig_img, test_img)

    print(f"\n=======================================================", flush=True)
    print(f"[TEST SMP APPEND] Starting {mode.upper()} (-smp {cpus}) Concurrent Append Suite", flush=True)
    print(f"=======================================================", flush=True)

    with tempfile.TemporaryDirectory(prefix="fortress-smp-uart-") as tmp:
        uart_path = Path(tmp) / "uart"
        qemu_cmd = [
            "qemu-system-x86_64", "-M", "q35", "-m", "2G", "-smp", str(cpus),
            "-display", "none", "-no-reboot", "-monitor", "none",
            "-boot", "d", "-cdrom", "bin/fortress.iso",
            "-chardev", f"socket,id=uart,path={uart_path},server=on,wait=off,logfile={log_path}",
            "-serial", "chardev:uart",
            "-drive", f"file={test_img},if=none,id=nvm0,format=raw,snapshot=off",
            "-device", "nvme,serial=fortress0,drive=nvm0",
            "-fw_cfg", "name=opt/fortress/write_test,string=1",
            "-fw_cfg", "name=opt/fortress/smp_append_test,string=1"
        ]

        if mode == "uefi":
            if not CODE.exists() or not VARS.exists():
                raise SystemExit("UEFI verification requires paired OVMF 4M firmware")
            vars_file = Path(tmp) / "vars.fd"
            shutil.copyfile(VARS, vars_file)
            qemu_cmd += [
                "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={CODE}",
                "-drive", f"if=pflash,format=raw,unit=1,file={vars_file}"
            ]

        child = subprocess.Popen(qemu_cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            uart = connect(uart_path)
            uart.settimeout(0.2)
            stop_drain = threading.Event()

            def drain():
                while not stop_drain.is_set():
                    try:
                        if not uart.recv(65536): return
                    except socket.timeout:
                        continue

            drain_thread = threading.Thread(target=drain, daemon=True)
            drain_thread.start()

            def output():
                if not log_path.exists(): return ""
                return log_path.read_text(errors="replace").replace("\r", "")

            def wait_for(pattern, timeout=45):
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    txt = output()
                    if pattern in txt:
                        time.sleep(0.05)
                        return txt
                    assert child.poll() is None, child.stderr.read().decode()
                    time.sleep(0.05)
                raise AssertionError(f"Timeout waiting for {pattern!r} in serial log:\n{output()[-1500:]}")

            def send_str(s):
                for b in s.encode():
                    uart.send(bytes([b]))
                    time.sleep(0.005)

            # Wait for test completion milestones in kernel log
            print(f"[{mode.upper()}] Waiting for SMP ext2 concurrent append milestones...", flush=True)
            wait_for("SMP ext2 Concurrent Append Verification (Finding 5)")
            wait_for("[ OK ] SMP ext2 concurrent append verification complete.")

            # Wait for shell prompt
            wait_for("fortress> ")
            print(f"[{mode.upper()}] Kernel tests completed; interactive shell ready.", flush=True)

            # Clean shutdown: send poweroff
            print(f"[{mode.upper()}] Sending poweroff for clean NVMe flush and ACPI S5 shutdown...", flush=True)
            send_str("poweroff\n")

            exit_code = child.wait(timeout=15)
            assert exit_code == 0, f"QEMU exited with code {exit_code}"
            stop_drain.set()

            # Analyze output log
            out = output()
            assert "[FAIL]" not in out, f"Kernel reported failures in serial log:\n{out}"

            # Check independent handles pass
            indep_pass = "Scenario: independent handles" in out and \
                         "200 records intact (3200 bytes), 0 lost, 0 duplicate, strict ordering preserved" in out
            assert indep_pass, "Missing or failed independent handles scenario verification"

            # Check shared handle pass
            shared_pass = "Scenario: shared handle" in out and \
                          "200 records intact (3200 bytes), 0 lost, 0 duplicate, strict ordering preserved" in out
            assert shared_pass, "Missing or failed shared handle scenario verification"

            # Extract interleaving transitions
            transitions = re.findall(r"Interleaving transitions: (\d+)", out)
            print(f"[{mode.upper()}] Interleaving transitions observed: {transitions}", flush=True)

            # Offline filesystem audit
            print(f"[{mode.upper()}] Running offline host e2fsck -fn on persisted partition...", flush=True)
            check_e2fsck(test_img)
            print(f"[{mode.upper()}] e2fsck verified clean ext2 filesystem with 0 errors.", flush=True)
            print(f"[PASS] {mode.upper()} (-smp {cpus}) SMP ext2 concurrent append verified.", flush=True)

        finally:
            if child.poll() is None:
                child.kill()
                child.wait()


def main():
    print("========================================================")
    print("SMP ext2 Concurrent Append Test Suite (Finding 5)")
    print("========================================================")

    for mode in ("bios", "uefi"):
        run_smp_append_test(mode=mode, cpus=4)

    print("\n[ALL PASS] SMP ext2 concurrent append verification complete across BIOS & UEFI.")


if __name__ == "__main__":
    main()
