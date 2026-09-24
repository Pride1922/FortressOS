#!/usr/bin/env python3
"""SMP Piece 5: Cross-Core Coordination & IPIs verification runner.

Verifies:
1. BIOS and UEFI single-CPU baseline (-smp 1):
   - Single-CPU skip cleanly logged
   - Shell prompt reached
2. BIOS and UEFI multi-core execution (-smp 4 and -smp 8):
   - Unicast IPI delivery (BSP -> AP 1)
   - Synchronous broadcast TLB shootdown acknowledged by all online APs
   - Remote core wakeup via reschedule IPI (AP 1 awakened from idle)
   - Real VMM page unmap and synchronous shootdown barrier (SM14, SM15)
   - Interactive shell prompt reached
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

REPO = Path(__file__).resolve().parent.parent


def run_ipi_test(mode, cpus):
    log = REPO / "build" / f"smp-ipi-{mode}-{cpus}.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="fortress-ipi-") as temp:
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
        if cpus == 1:
            assert "Single-CPU system: multi-core IPI tests skipped." in output, "Single CPU skip missing"
        else:
            assert "Unicast IPI delivery verified (BSP -> AP 1)" in output, \
                f"Unicast IPI delivery missing for {cpus} cores"
            assert f"Synchronous broadcast TLB shootdown acknowledged by all online APs ({cpus - 1} APs)" in output, \
                f"Broadcast TLB shootdown missing for {cpus} cores"
            assert "Remote core wakeup verified (AP 1 awakened from idle)" in output, \
                f"Remote core wakeup missing for {cpus} cores"
            assert "VMM synchronous TLB shootdown and frame unmap verified (SM14, SM15)" in output, \
                f"VMM synchronous shootdown missing for {cpus} cores"
        assert "[ OK ] SMP Piece 5 (Cross-Core Coordination & IPIs) complete." in output, "Piece 5 completion missing"
        print(f"PASS {mode} -smp {cpus}: unicast, broadcast TLB shootdown, remote wake, VMM unmap, shell reached")


def main():
    print("========================================================")
    print("SMP Piece 5: Cross-Core Coordination & IPIs Test Suite")
    print("========================================================")
    for cpus in (1, 4, 8):
        for mode in ("bios", "uefi"):
            run_ipi_test(mode, cpus)
    print("\n[ALL PASS] SMP Piece 5 verification complete across BIOS & UEFI (1, 4, 8 CPUs).")


if __name__ == "__main__":
    main()
