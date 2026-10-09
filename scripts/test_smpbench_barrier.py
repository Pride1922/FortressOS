#!/usr/bin/env python3
"""Finite BIOS/UEFI readiness checks with panic capture; ISO only, no data disks."""
import argparse
import json
import subprocess
from datetime import datetime, timezone
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cpus", nargs="+", type=int, choices=(1, 4, 8), default=[1, 4, 8])
parser.add_argument("--firmware", nargs="+", choices=("bios", "uefi"), default=["bios", "uefi"])
parser.add_argument("--reps", type=int, default=3)
parser.add_argument("--output", type=Path)
parser.add_argument("--profile", action="store_true", help="Validate opt-in phase accounting as well as readiness.")
args = parser.parse_args()
assert 1 <= args.reps <= 31
directory = args.output or REPO / "build" / ("barrier-" + datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S"))
directory.mkdir(parents=True, exist_ok=False)
for firmware in args.firmware:
    for cpus in args.cpus:
        evidence = directory / f"{firmware}-smp{cpus}"
        subprocess.run(["python3", "scripts/capture_smp_panic.py", "--iso", "bin/fortress.iso",
                        "--elf", "bin/fortress.elf", "--output", str(evidence),
                        "--firmware", firmware, "--cpus", str(cpus), "--runs", "1",
                        "--reps", str(args.reps), "--require-barrier", "--dump-ram",
                        *(["--profile"] if args.profile else [])], cwd=REPO, check=True)
        manifest = json.loads((evidence / "manifest.json").read_text())
        assert len(manifest["runs"]) == 1
        assert manifest["runs"][0]["outcome"] == "no-panic-observed", manifest["runs"][0]
        print(f"Barrier/checksum PASS: {firmware} SMP={cpus}", flush=True)
