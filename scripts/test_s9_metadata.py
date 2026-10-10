#!/usr/bin/env python3
"""Phase 0: real all-CPU Ring 3 accounting; disposable ISO, no data disks."""
from pathlib import Path
import shutil
import re
import subprocess
import tempfile
import time

REPO = Path(__file__).resolve().parent.parent


def command(mode, cpus, iso, variables, log):
    cmd = ["qemu-system-x86_64", "-accel", "tcg", "-M", "q35", "-m", "2G",
           "-smp", str(cpus), "-display", "none", "-monitor", "none", "-no-reboot",
           "-serial", f"file:{log}", "-boot", "d", "-cdrom", str(iso),
           "-fw_cfg", "name=opt/fortress/s9_metadata_test,string=1"]
    if mode == "uefi":
        cmd += ["-drive", "if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
                "-drive", f"if=pflash,format=raw,unit=1,file={variables}"]
    return cmd


def preflight(cmd, mode, cpus, iso, variables, log):
    # Exact grammar: permits neither alternate backends nor trailing arguments.
    assert mode in ("bios", "uefi") and cpus in (1, 4, 8)
    assert cmd == command(mode, cpus, iso, variables, log), "unexpected QEMU argv"


def main():
    with tempfile.TemporaryDirectory(prefix="fortress-s9-metadata-") as directory:
        tmp = Path(directory)
        iso, variables = tmp / "metadata.iso", tmp / "vars.fd"
        shutil.copyfile(REPO / "bin/fortress.iso", iso)
        for cpus in (1, 4, 8):
            for mode in ("bios", "uefi"):
                log = REPO / "build" / f"s9-metadata-{mode}-{cpus}.log"
                log.write_text("")
                if mode == "uefi":
                    shutil.copyfile("/usr/share/OVMF/OVMF_VARS_4M.fd", variables)
                cmd = command(mode, cpus, iso, variables, log)
                preflight(cmd, mode, cpus, iso, variables, log)
                for extra in (["-drive", "file=unsafe.img"], ["-blockdev", "driver=file,filename=unsafe.img"],
                              ["-device", "nvme"], ["-hda", "unsafe.img"], ["-snapshot"]):
                    try:
                        preflight(cmd + extra, mode, cpus, iso, variables, log)
                    except AssertionError:
                        pass
                    else:
                        raise AssertionError("preflight accepted injection")
                with log.with_suffix(".stderr").open("wb") as stderr:
                    child = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=stderr)
                    try:
                        deadline = time.monotonic() + 180
                        while time.monotonic() < deadline:
                            output = log.read_text(errors="replace")
                            assert "S9 METADATA FAIL" not in output and "[FATAL]" not in output, output[-4000:]
                            if "S9 METADATA PASS" in output and re.search(
                                    r"(?:fortress> |fortress:[^\r\n]* \$ )", output):
                                break
                            assert child.poll() is None, output[-4000:]
                            time.sleep(0.1)
                        else:
                            raise TimeoutError(output[-4000:])
                    finally:
                        child.terminate()
                        try:
                            child.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            child.kill()
                            child.wait(timeout=5)
                for cpu in range(cpus):
                    assert f"S9 METADATA CPU {cpu} ticks=" in output
                if cpus > 1:
                    assert f"verified {cpus} cores" in output
                print(f"PASS S9 metadata {mode} SMP={cpus}: all owner CPUs, concurrent reader, final ticks, zombie/reap/wait, prompt", flush=True)
    print("PASS S9 argv preflight: extra drive/blockdev/device/hda/snapshot rejected")


if __name__ == "__main__":
    main()
