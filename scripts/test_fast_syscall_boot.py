#!/usr/bin/env python3
"""Exercise the ordinary live boot's fast-syscall gate; no data disks attached."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time

REPO = Path(__file__).resolve().parent.parent
DONE = "[ OK ] Phase 7 (Checkpoint 5) completed successfully!"


def boot(firmware, cpus, accel, output, iso, probe):
    name = f"{firmware}-{accel}-smp{cpus}-{probe}"
    serial = output / f"{name}.log"
    errors = output / f"{name}.stderr"
    with tempfile.TemporaryDirectory(prefix="fortress-fast-") as directory:
        temp = Path(directory)
        command = ["qemu-system-x86_64", "-machine", "q35", "-accel", accel,
                   "-cpu", "host" if accel == "kvm" else "max", "-m", "2G",
                   "-smp", str(cpus), "-display", "none", "-monitor", "none",
                   "-serial", f"file:{serial}", "-no-reboot", "-boot", "d",
                   "-cdrom", str(iso)]
        if firmware == "uefi":
            shutil.copyfile("/usr/share/OVMF/OVMF_VARS_4M.fd", temp / "vars.fd")
            command += ["-drive", "if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
                        "-drive", f"if=pflash,format=raw,unit=1,file={temp}/vars.fd"]
        # Only ISO and private firmware variable storage: no fixture/physical disks.
        assert command.count("-drive") == (2 if firmware == "uefi" else 0)
        with errors.open("w") as stderr:
            child = subprocess.Popen(command, cwd=REPO, stdout=subprocess.DEVNULL, stderr=stderr)
            try:
                deadline = time.monotonic() + 120
                while time.monotonic() < deadline:
                    log = serial.read_text(errors="replace") if serial.exists() else ""
                    # UART writes arrive incrementally; judge failures only once
                    # their complete diagnostic line has reached the log.
                    complete = log.rsplit("\n", 1)[0] if "\n" in log else ""
                    if probe == "timeout" and "[FAIL] Repeated fast syscalls under preemption failed! Codes: P1=17, P2=17" in complete:
                        assert DONE not in log
                        print(f"PASS {name}: injected expired deadline rejected both workers", flush=True)
                        return
                    if DONE in log:
                        break
                    assert "[FAIL]" not in complete, f"{name}: boot failure; see {serial}"
                    assert child.poll() is None, f"{name}: QEMU exited; see {errors}"
                    time.sleep(0.1)
                else:
                    raise AssertionError(f"{name}: boot timeout; see {serial}")
                gate = log.split("Phase 7 (Checkpoint 5):", 1)[1].split(DONE, 1)[0]
                assert "[FAIL]" not in gate
                workers = re.findall(r"completed 60 fast syscalls \(Preemptions=(\d+), CPU ticks=(\d+), Exit=(91|92)\)", gate)
                assert len(workers) == 2 and {w[2] for w in workers} == {"91", "92"}, gate
                assert all(int(p) > 0 and int(t) >= 8 for p, t, _ in workers), workers
                switches = re.search(r"(\d+) runnable context switches", gate)
                assert switches and int(switches[1]) >= 2, gate
                assert "All dedicated kernel stack slots cleanly recycled" in gate
                assert "Dynamic kernel heap integrity walk passed" in gate
                print(f"PASS {name}: workers={workers}, runnable switches={switches[1]}", flush=True)
            finally:
                child.terminate()
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()


def probe_iso(directory, probe):
    """Patch only a private embedded worker copy; release media stays intact."""
    source = (REPO / "build/init.elf").read_bytes()
    kernel = (REPO / "bin/fortress.elf").read_bytes()
    assert kernel.count(source) == 1, "Expected one embedded init ELF"
    if probe == "fast":
        # Reduce each worker's compute loop 500,000-fold. A fixed-workload gate
        # would finish before a timeslice; the tick-based gate must still pass.
        before = bytes.fromhex("b920a1070048ff05")
        after = bytes.fromhex("b90100000048ff05")
        assert source.count(before) == 2, "Worker instruction signature changed"
    else:
        # Force the monotonic deadline to be already expired (cmp elapsed, 0).
        # This checks fail-closed propagation, not hardware timer failure.
        before = bytes.fromhex("4c29f04883f80a")
        after = bytes.fromhex("4c29f04883f800")
        assert source.count(before) == 1, "Deadline instruction signature changed"
    patched = directory / "fortress.elf"
    patched.write_bytes(kernel.replace(source, source.replace(before, after)))
    iso = directory / "probe.iso"
    subprocess.run(["xorriso", "-indev", str(REPO / "bin/fortress.iso"),
                    "-outdev", str(iso), "-boot_image", "any", "replay",
                    "-map", str(patched), "/boot/fortress.elf"], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return iso


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--accel", choices=("tcg", "kvm"), default="tcg")
    parser.add_argument("--probe", choices=("normal", "fast", "timeout"), default="normal")
    args = parser.parse_args()
    output = REPO / "build/fast-syscall-boot"
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="fortress-fast-probe-") as directory:
        iso = REPO / "bin/fortress.iso"
        if args.probe != "normal":
            iso = probe_iso(Path(directory), args.probe)
        for firmware in ("bios", "uefi"):
            for cpus in (1, 4):
                boot(firmware, cpus, args.accel, output, iso, args.probe)


if __name__ == "__main__":
    main()
