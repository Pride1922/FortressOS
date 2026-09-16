#!/usr/bin/env python3
"""Boot the acceptance suite in both firmware modes using snapshot disk writes."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

repo = Path(__file__).resolve().parent.parent
markers = [
    "Phase 9 (Step 9C.1): GPT Partition Parsing & Block Devices PASSED!",
    "[PASS] Exact bitmap detects changed allocation set despite equal allocation counters",
    "[PASS] Ring 3 ext2: 10 audited cycles, exact PMM bitmap restored, stable kernel mappings and heap",
    "[BOOT] FortressOS Phase 9 (Step 9C.2) complete.",
]
for mode in ("bios", "uefi"):
    log = repo / "build" / f"storage-{mode}.log"
    with tempfile.TemporaryDirectory(prefix="fortress-ovmf-") as tmp:
        cmd = ["qemu-system-x86_64", "-M", "q35", "-m", "2G", "-display", "none",
               "-serial", f"file:{log}", "-monitor", "none", "-no-reboot",
               "-drive", "file=build/nvme_gpt.img,if=none,id=nvm0,format=raw,snapshot=on",
               "-device", "nvme,serial=fortress0,drive=nvm0", "-boot", "d",
               "-cdrom", "bin/fortress.iso"]
        if mode == "uefi":
            code = Path("/usr/share/OVMF/OVMF_CODE_4M.fd")
            source = Path("/usr/share/OVMF/OVMF_VARS_4M.fd")
            if not code.exists() or not source.exists():
                raise SystemExit("UEFI verification requires paired OVMF 4M firmware")
            vars_file = Path(tmp) / "vars.fd"
            shutil.copyfile(source, vars_file)
            cmd += ["-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={code}",
                    "-drive", f"if=pflash,format=raw,unit=1,file={vars_file}"]
        with log.open("w"):
            pass
        child = subprocess.Popen(cmd, cwd=repo, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 90
            while time.monotonic() < deadline:
                output = log.read_text(errors="replace")
                if markers[-1] in output or child.poll() is not None:
                    break
                time.sleep(0.2)
            missing = [m for m in markers if m not in log.read_text(errors="replace")]
            if missing:
                raise SystemExit(f"FAIL {mode}: missing {missing}; inspect {log}")
            print(f"PASS {mode}: GPT, ext2 Ring 3 and allocation-set audits ({log})", flush=True)
        finally:
            child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
