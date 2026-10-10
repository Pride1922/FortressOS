#!/usr/bin/env python3
"""One-command memory correctness campaign with retained commands, hashes and logs."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent


def hashes():
    result = {}
    for name in ("bin/fortress.elf", "bin/fortress.iso", "bin/fortress.img",
                 "docs/plans/PERMISSIONS_PLAN.md"):
        path = ROOT / name
        if path.is_file():
            with path.open("rb") as stream:
                result[name] = hashlib.file_digest(stream, "sha256").hexdigest()
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host-only", action="store_true")
    args = parser.parse_args()
    out = ROOT / "build" / ("memory-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ"))
    out.mkdir(parents=True, exist_ok=False)
    record = {"commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
              "status_before": subprocess.check_output(["git", "status", "--short"], cwd=ROOT, text=True),
              "hashes_before": hashes(), "steps": [], "status": "RUNNING"}

    def save():
        (out / "result.json").write_text(json.dumps(record, indent=2) + "\n")

    def run(name, command, timeout):
        log = out / f"{name}.log"
        step = {"name": name, "argv": command, "log": str(log), "status": "RUNNING"}
        record["steps"].append(step)
        save()
        print(f"RUN {name}; {log}", flush=True)
        start = time.monotonic()
        started_wall = time.time()
        try:
            with log.open("w") as stream:
                child = subprocess.run(command, cwd=ROOT, stdout=stream,
                                       stderr=subprocess.STDOUT, timeout=timeout)
            step["returncode"] = child.returncode
            step["status"] = "PASS" if child.returncode == 0 else "FAIL"
            if child.returncode:
                raise RuntimeError(f"{name} failed; see {log}")
        except Exception as error:
            step["status"] = "FAIL"
            step["error"] = str(error)
            raise
        finally:
            step["elapsed_seconds"] = round(time.monotonic() - start, 3)
            patterns = {"vmm-qemu": "smp-vmm-lifecycle-*",
                        "boot-low-ram": "smp-memory-boot-*-256M.*"}
            if name in patterns:
                detail = out / name
                detail.mkdir(exist_ok=True)
                for path in (ROOT / "build").glob(patterns[name]):
                    if path.is_file() and path.stat().st_mtime >= started_wall:
                        shutil.copyfile(path, detail / path.name)
            save()

    save()
    print(f"Evidence: {out}", flush=True)
    try:
        run("host", ["make", "test-heap-memory-host", "test-pmm-audit-host",
                     "test-pmm-boot-host", "test-vmm-host", "test-smp-memory-host"], 180)
        if not args.host_only:
            run("build", ["make", "-j4"], 600)
            run("vmm-qemu", [sys.executable, "scripts/test_smp_vmm.py", "--timeout", "90"], 600)
            run("boot-low-ram", [sys.executable, "scripts/test_smp_memory_boot.py",
                "--ram", "256M", "--cpus", "1", "4", "--timeout", "90"], 420)
            run("ext4-memory", [sys.executable, "scripts/test_memory_ext4.py",
                "--output", str(out / "ext4"), "--cpus", "4"], 600)
        record["status"] = "PASS"
    except Exception as error:
        record["status"] = "FAIL"
        record["error"] = str(error)
        raise
    finally:
        record["hashes_after"] = hashes()
        assert record["hashes_before"].get("docs/plans/PERMISSIONS_PLAN.md") == record["hashes_after"].get("docs/plans/PERMISSIONS_PLAN.md")
        save()
        print(f'{record["status"]}: {out / "result.json"}', flush=True)


if __name__ == "__main__":
    main()
