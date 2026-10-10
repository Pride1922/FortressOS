#!/usr/bin/env python3
"""Read-only memory observability acceptance test: SYS_MEMINFO and sysinfo -m."""
import datetime
import json
from pathlib import Path
import re
import shutil
import socket
import subprocess
import tempfile
import threading
import time

REPO = Path(__file__).resolve().parent.parent
CODE = Path("/usr/share/OVMF/OVMF_CODE_4M.fd")
VARS = Path("/usr/share/OVMF/OVMF_VARS_4M.fd")
PROMPT_PATTERN = r"(?:fortress> |(?:\[-?\d+\] )?fortress:[^\r\n]* \$ |\[[a-zA-Z0-9_\-\./]+\]# )"


def digest(path):
    import hashlib
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    out = REPO / "build" / ("memory-observability-" + datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ"))
    out.mkdir(parents=True)
    tracked = [REPO / "bin/fortress.img", REPO / "bin/fortress.elf", REPO / "docs/plans/PERMISSIONS_PLAN.md"]
    before = {str(p): digest(p) for p in tracked}
    record = {"status": "FAIL", "before": before, "cases": []}

    iso_src = REPO / "bin/fortress.iso"
    assert iso_src.is_file(), f"Missing production ISO: {iso_src}"
    iso = out / "memory-observability.iso"
    shutil.copyfile(iso_src, iso)

    cases_config = [
        ("bios", "256M", True),
        ("uefi", "256M", True),
        ("bios", "512M", True),
        ("uefi", "512M", True),
        ("bios", "256M", False),
    ]

    all_passed = True

    try:
        with tempfile.TemporaryDirectory(prefix="fortress-mem-sock-") as sock_dir:
            sock_base = Path(sock_dir)
            for firmware, ram, enabled in cases_config:
                label = f"{firmware}-{ram}-" + ("observability" if enabled else "control")
                uart_log = out / f"{label}.log"
                uart_log.write_text("")
                stderr_log = out / f"{label}.stderr"
                uart_sock = sock_base / f"{label}.sock"
                vars_fd = out / f"{label}-vars.fd"

                command = [
                    "qemu-system-x86_64", "-accel", "tcg", "-M", "q35", "-m", ram,
                    "-smp", "1", "-display", "none", "-monitor", "none", "-no-reboot",
                    "-boot", "d", "-cdrom", str(iso),
                    "-chardev", f"socket,id=uart,path={uart_sock},server=on,wait=off,logfile={uart_log}",
                    "-serial", "chardev:uart"
                ]

                drives = []
                if firmware == "uefi":
                    assert CODE.is_file() and VARS.is_file()
                    shutil.copyfile(VARS, vars_fd)
                    drives = [
                        f"if=pflash,format=raw,unit=0,readonly=on,file={CODE}",
                        f"if=pflash,format=raw,unit=1,file={vars_fd}"
                    ]
                    for drive in drives:
                        command += ["-drive", drive]

                # Preflight assertions
                assert not any(a in command for a in ("-device", "-blockdev", "-hda", "-hdb", "nvme"))
                (out / f"{label}.argv.json").write_text(json.dumps(command, indent=2) + "\n")

                case = {
                    "firmware": firmware,
                    "ram": ram,
                    "enabled": enabled,
                    "argv": command,
                    "status": "FAIL"
                }
                record["cases"].append(case)

                start = time.monotonic()
                with stderr_log.open("w") as err:
                    child = subprocess.Popen(command, cwd=REPO, stdout=subprocess.DEVNULL, stderr=err)
                    sock = None
                    stop_drain = threading.Event()
                    try:
                        # Wait for socket to become available
                        deadline = time.monotonic() + 30
                        while time.monotonic() < deadline:
                            if uart_sock.exists():
                                try:
                                    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                                    sock.connect(str(uart_sock))
                                    break
                                except (ConnectionRefusedError, OSError):
                                    pass
                            assert child.poll() is None, f"QEMU exited early: {stderr_log}"
                            time.sleep(0.05)
                        assert sock is not None, f"Failed to connect to UART socket: {uart_sock}"
                        sock.settimeout(0.2)

                        def drain_socket():
                            while not stop_drain.is_set():
                                try:
                                    if not sock.recv(65536):
                                        return
                                except (socket.timeout, OSError):
                                    pass

                        drain_thread = threading.Thread(target=drain_socket, daemon=True)
                        drain_thread.start()

                        def get_clean_text():
                            raw = uart_log.read_bytes().decode(errors="replace")
                            # Strip ANSI escape sequences and carriage returns
                            return re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "", raw).replace("\r", "")

                        def wait_prompt(after_pos=0, timeout=60):
                            t_end = time.monotonic() + timeout
                            while time.monotonic() < t_end:
                                txt = get_clean_text()
                                if re.search(PROMPT_PATTERN, txt[after_pos:]):
                                    return txt[after_pos:]
                                assert child.poll() is None, "QEMU terminated prematurely"
                                time.sleep(0.05)
                            raise TimeoutError(f"Prompt wait timed out: {get_clean_text()[-500:]}")

                        def send_cmd(cmd_str):
                            pos_start = len(get_clean_text())
                            for b in cmd_str.encode() + b"\n":
                                sock.send(bytes([b]))
                                time.sleep(0.005)
                            return wait_prompt(pos_start)

                        # 1. Wait for initial shell prompt
                        wait_prompt(0, timeout=60)

                        if enabled:
                            # 2. Test standard sysinfo (default overview backward-compatibility)
                            out_std = send_cmd("/bin/sysinfo")
                            assert "FortressOS 1.0 (x86_64 SMP, 1 CPU)" in out_std, f"Unexpected std sysinfo: {out_std}"
                            assert "RAM:  total" in out_std and "Heap:" in out_std, f"Missing RAM/Heap in std: {out_std}"

                            # 3. Test sysinfo -m (detailed memory subsystem observability)
                            out_mem = send_cmd("/bin/sysinfo -m")
                            assert "FortressOS Memory Subsystem Observability" in out_mem, f"Missing header: {out_mem}"
                            assert "Physical Memory (PMM):" in out_mem, f"Missing PMM: {out_mem}"
                            assert "Kernel Dynamic Heap:" in out_mem, f"Missing Heap: {out_mem}"
                            assert "Virtual Memory Management (VMM):" in out_mem, f"Missing VMM: {out_mem}"
                            assert "Snapshot Notice:" in out_mem, f"Missing notice: {out_mem}"

                            # Parse PMM
                            pmm_tot_m = re.search(r"Total managed:\s+(\d+)\s+frames", out_mem)
                            pmm_used_m = re.search(r"Used / allocated:\s+(\d+)\s+frames", out_mem)
                            pmm_free_m = re.search(r"Free / available:\s+(\d+)\s+frames", out_mem)
                            pmm_alloc_m = re.search(r"Allocatable \(<1G\):\s+(\d+)\s+frames", out_mem)
                            assert pmm_tot_m and pmm_used_m and pmm_free_m and pmm_alloc_m, f"Failed parsing PMM: {out_mem}"

                            p_tot = int(pmm_tot_m.group(1))
                            p_used = int(pmm_used_m.group(1))
                            p_free = int(pmm_free_m.group(1))
                            p_alloc = int(pmm_alloc_m.group(1))

                            assert p_tot > 0 and p_used > 0 and p_free > 0, f"Non-positive PMM values: {p_tot}, {p_used}, {p_free}"
                            assert p_used + p_free == p_tot, f"PMM used+free != total: {p_used}+{p_free} != {p_tot}"
                            assert p_alloc <= p_free, f"Allocatable > free: {p_alloc} > {p_free}"

                            # Parse Heap
                            heap_used_m = re.search(r"Live used:\s+(\d+)\s+B \(including 32B block metadata\)", out_mem)
                            heap_free_m = re.search(r"Reusable free:\s+(\d+)\s+B \(within committed capacity\)", out_mem)
                            heap_comm_m = re.search(r"Committed backing:\s+(\d+)\s+B \((\d+) physical frames\)", out_mem)
                            heap_lg_m = re.search(r"Largest free chunk:\s+(\d+)\s+B payload \(excludes block tags\)", out_mem)
                            heap_blk_m = re.search(r"Free blocks count:\s+(\d+)", out_mem)
                            assert heap_used_m and heap_free_m and heap_comm_m and heap_lg_m and heap_blk_m, f"Failed parsing Heap: {out_mem}"

                            h_used = int(heap_used_m.group(1))
                            h_free = int(heap_free_m.group(1))
                            h_comm = int(heap_comm_m.group(1))
                            h_frames = int(heap_comm_m.group(2))
                            h_lg = int(heap_lg_m.group(1))
                            h_blk = int(heap_blk_m.group(1))

                            assert h_used > 0 and h_free > 0 and h_comm > 0, f"Non-positive Heap values: {h_used}, {h_free}, {h_comm}"
                            assert h_used + h_free == h_comm, f"Heap used+free != committed: {h_used}+{h_free} != {h_comm}"
                            assert h_comm == h_frames * 4096, f"Heap frames != committed/4096: {h_frames} vs {h_comm}"
                            assert h_lg <= h_free, f"Largest payload > free bytes: {h_lg} > {h_free}"
                            assert h_blk >= 1, f"Zero free blocks: {h_blk}"

                            # Parse VMM
                            vmm_tbl_m = re.search(r"Page-table frames:\s+(\d+)\s+frames \(kernel \+ user hierarchy\)", out_mem)
                            vmm_def_m = re.search(r"Deferred teardown:\s+(\d+)\s+queued address spaces", out_mem)
                            assert vmm_tbl_m and vmm_def_m, f"Failed parsing VMM: {out_mem}"

                            v_tbl = int(vmm_tbl_m.group(1))
                            v_def = int(vmm_def_m.group(1))

                            assert v_tbl > 0, f"Zero table frames: {v_tbl}"
                            assert v_def == 0, f"Deferred address spaces not empty at quiescence: {v_def}"

                            case["observability"] = {
                                "pmm": {"total": p_tot, "used": p_used, "free": p_free, "allocatable": p_alloc},
                                "heap": {"used_bytes": h_used, "free_bytes": h_free, "committed_bytes": h_comm,
                                         "largest_payload": h_lg, "free_blocks": h_blk},
                                "vmm": {"table_frames": v_tbl, "deferred_spaces": v_def}
                            }

                            # 4. Process lifecycle observation: run ps, verify sysinfo -m stays sane
                            out_ps = send_cmd("/bin/ps")
                            assert "PID" in out_ps or "init" in out_ps, f"Unexpected ps output: {out_ps}"

                            out_mem2 = send_cmd("/bin/sysinfo -m")
                            pmm_used2_m = re.search(r"Used / allocated:\s+(\d+)\s+frames", out_mem2)
                            pmm_free2_m = re.search(r"Free / available:\s+(\d+)\s+frames", out_mem2)
                            heap_used2_m = re.search(r"Live used:\s+(\d+)\s+B", out_mem2)
                            heap_comm2_m = re.search(r"Committed backing:\s+(\d+)\s+B", out_mem2)
                            assert pmm_used2_m and pmm_free2_m and heap_used2_m and heap_comm2_m
                            assert int(pmm_used2_m.group(1)) + int(pmm_free2_m.group(1)) == p_tot
                            assert int(heap_used2_m.group(1)) <= int(heap_comm2_m.group(1))

                            # 5. Shell recovery test
                            out_rec = send_cmd("echo MEM_OBS_RECOVERY_OK")
                            assert "MEM_OBS_RECOVERY_OK" in out_rec, f"Shell recovery failed: {out_rec}"

                        else:
                            # Control boot: verify clean shell interaction
                            out_ctrl = send_cmd("echo CONTROL_BOOT_OK")
                            assert "CONTROL_BOOT_OK" in out_ctrl, f"Control shell failed: {out_ctrl}"

                        case["status"] = "PASS"
                        case["elapsed_seconds"] = round(time.monotonic() - start, 2)
                        print(f"[{case['status']}] {label} ({case['elapsed_seconds']}s)")

                    finally:
                        stop_drain.set()
                        if sock:
                            try:
                                sock.close()
                            except Exception:
                                pass
                        if child.poll() is None:
                            child.terminate()
                        try:
                            child.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            child.kill()
                            child.wait(timeout=5)

                if case["status"] != "PASS":
                    all_passed = False

            if all_passed:
                record["status"] = "PASS"

    finally:
        after = {str(p): digest(p) for p in tracked}
        record["after"] = after
        assert before == after, f"Tracked artifact mutation: before={before} after={after}"
        (out / "result.json").write_text(json.dumps(record, indent=2) + "\n")
        print(f"\nResult saved to: {out / 'result.json'}")

    assert record["status"] == "PASS", f"Test failed; see {out / 'result.json'}"
    print(f"\nPASS test-memory-observability: All 5 cases passed cleanly!")


if __name__ == "__main__":
    main()
