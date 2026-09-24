#!/usr/bin/env python3
"""SMP Piece 4: The SMP Scheduler & Work-Stealing verification runner.

Verifies:
1. BIOS and UEFI single-CPU baseline (-smp 1):
   - Dual-lock forward and inverted ordering (sched_lock_pair) passes
   - Single-CPU skip cleanly logged
   - Shell prompt reached
2. BIOS and UEFI multi-core execution (-smp 4 and -smp 8):
   - Dual-lock ordering passes
   - Pinned workers execute concurrently on their assigned CPU cores
   - Unbound workers queued on CPU 0 are actively stolen by idle APs
   - Reaches interactive shell prompt
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

REPO = Path(__file__).resolve().parent.parent


def run_sched_test(mode, cpus):
    log = REPO / "build" / f"smp-sched-{mode}-{cpus}.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="fortress-sched-") as temp:
        temp = Path(temp)
        cmd = ["qemu-system-x86_64", "-accel", "tcg", "-M", "q35", "-m", "2G",
               "-smp", str(cpus), "-display", "none", "-no-reboot", "-monitor", "none",
               "-serial", f"file:{log}", "-boot", "d", "-cdrom", "bin/fortress.iso"]
        if mode == "uefi":
            shutil.copyfile("/usr/share/OVMF/OVMF_VARS_4M.fd", temp / "vars.fd")
            cmd += ["-drive", "if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
                    "-drive", f"if=pflash,format=raw,unit=1,file={temp}/vars.fd"]
        child = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline:
                if log.exists() and "fortress> " in log.read_text(errors="replace"):
                    break
                assert child.poll() is None, child.stderr.read().decode()
                time.sleep(0.1)
            else:
                raise TimeoutError(f"Shell prompt not reached in {mode} -smp {cpus}")
        finally:
            child.terminate()
            child.wait()

        output = log.read_text(errors="replace")
        assert "sched_lock_pair acquired and released safely in forward and inverted order" in output, \
            "Dual-lock ordering test missing"
        if cpus == 1:
            assert "Single-CPU system: multi-core scheduler tests skipped." in output, "Single CPU skip missing"
        else:
            assert f"All pinned workers executed on their assigned CPU cores (verified {cpus} cores)" in output, \
                f"Pinned worker execution missing for {cpus} cores"
            assert "AP work-stealing verified" in output, "Work-stealing verification missing"
        assert "[ OK ] SMP Piece 4 (Scheduler & Work-Stealing) complete." in output, "Piece 4 completion missing"
        print(f"PASS {mode} -smp {cpus}: dual-lock ordering, pinned execution, work-stealing={cpus > 1}, shell reached")


def main():
    print("========================================================")
    print("SMP Piece 4: Scheduler & Work-Stealing Test Suite")
    print("========================================================")
    for cpus in (1, 4, 8):
        for mode in ("bios", "uefi"):
            run_sched_test(mode, cpus)
    print("\n[ALL PASS] SMP Piece 4 verification complete across BIOS & UEFI (1, 4, 8 CPUs).")


if __name__ == "__main__":
    main()
