#!/usr/bin/env python3
import os
import tempfile
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def main():
    out = ROOT / "build/repo_public"
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="fortress-seed-") as td:
        seed_path = Path(td) / "signer.seed"
        seed_path.write_bytes(bytes(range(32)))
        seed_path.chmod(0o600)
        cmd = [
            "python3", "scripts/pkg_repository.py",
            "--repository", "00010203-0405-0607-0809-0a0b0c0d0e0f",
            "--root-version", "1",
            "--revision", "1",
            "--issued", "1770000000",
            "--expires", "1890000000",
            "--key", str(seed_path),
            "--artifact", "build/pkg-rot13/v1.fpk",
            "--output", str(out)
        ]
        subprocess.run(cmd, cwd=ROOT, check=True)
    print("Repository generated successfully in:", out)
    for p in out.rglob("*"):
        if p.is_file():
            print(" ", p.relative_to(out), f"({p.stat().st_size} bytes)")

if __name__ == "__main__":
    main()
