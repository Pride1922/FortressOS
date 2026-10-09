#!/usr/bin/env python3
"""Freeze verified benchmark artifacts and a Dell handoff. Regular files only; never flash."""
import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import tarfile
import tempfile
import uuid
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--verify-only", action="store_true", help="Recheck an existing frozen bundle without replacing its binaries or procedure.")
    args = parser.parse_args()
    out = args.output.resolve()
    assert out.is_relative_to(REPO / "build") and out != REPO / "build", "Use a new directory inside build"
    if args.verify_only:
        assert out.is_dir(), "Bundle does not exist"
    else:
        out.mkdir(parents=True, exist_ok=False)
    artifacts = {}
    for source, name in (("bin/fortress.img", "fortress-dell-smpbench.img"),
                         ("bin/fortress.iso", "fortress.iso"), ("bin/fortress.elf", "fortress.elf"),
                         ("bin/initramfs.tar", "initramfs.tar"), ("build/tool-smpbench.elf", "smpbench.elf")):
        if not args.verify_only:
            path = REPO / source
            assert path.is_file(), f"Build first: {source}"
            shutil.copyfile(path, out / name)
        artifacts[name] = {"sha256": digest(out / name), "bytes": (out / name).stat().st_size}
    image = out / "fortress-dell-smpbench.img"
    verification = subprocess.run(["python3", "scripts/create_boot_img.py", str(image), "--verify"],
                                  cwd=REPO, capture_output=True, text=True, check=True, timeout=60)
    (out / "image-verification.txt").write_text(verification.stdout + verification.stderr)
    with image.open("rb") as stream:
        stream.seek(512); header = stream.read(512)
        assert header[:8] == b"EFI PART"
        stream.seek(struct.unpack_from("<Q", header, 72)[0] * 512)
        entries = stream.read(256)
    esp_start = struct.unpack_from("<Q", entries, 32)[0]
    data_uuid = str(uuid.UUID(bytes_le=entries[128 + 16:128 + 32])).upper()
    with tempfile.TemporaryDirectory(prefix="fortress-dell-verify-") as temporary:
        temporary = Path(temporary)
        for member, name in (("boot/fortress.elf", "fortress.elf"), ("boot/initramfs.tar", "initramfs.tar")):
            extracted = temporary / name
            subprocess.run(["mcopy", "-i", f"{image}@@{esp_start * 512}", f"::/{member}", str(extracted)],
                           check=True, capture_output=True, timeout=30)
            assert digest(extracted) == artifacts[name]["sha256"], f"Raw image mismatch: {member}"
        config = temporary / "limine.conf"
        subprocess.run(["mcopy", "-i", f"{image}@@{esp_start * 512}", "::/boot/limine/limine.conf", str(config)],
                       check=True, capture_output=True, timeout=30)
        conf = config.read_text()
        assert f"usb_data=PARTUUID={data_uuid}" in conf and "usb_data_mode=ro" in conf and "usb_data_mode=rw" in conf
        (out / "image-limine.conf").write_text(conf)
        for name in ("fortress.elf", "initramfs.tar"):
            extracted = temporary / ("iso-" + name)
            subprocess.run(["xorriso", "-osirrox", "on", "-indev", str(out / "fortress.iso"),
                            "-extract", "/boot/" + name, str(extracted)], check=True, capture_output=True, timeout=30)
            assert digest(extracted) == artifacts[name]["sha256"], "ISO artifact mismatch: " + name
    with tarfile.open(out / "initramfs.tar") as archive:
        matches = [member for member in archive.getmembers() if member.name.removeprefix("./") == "bin/smpbench"]
        assert len(matches) == 1 and matches[0].isfile()
        binary = archive.extractfile(matches[0]).read()
    assert hashlib.sha256(binary).hexdigest() == artifacts["smpbench.elf"]["sha256"], "Embedded benchmark mismatch"
    assert b"rev=3 profile=1" in binary and b"barrier=pipe" in binary
    if args.verify_only:
        assert json.loads((out / "manifest.json").read_text())["artifacts"] == artifacts, "Bundle hash manifest mismatch"
        print(f"Frozen bundle hashes, GPT/filesystems and exact raw/ISO kernel/initramfs/benchmark contents verified: {out}")
        return
    commands = ["sha256sum /bin/smpbench > /mnt/dell-smp-00-toolhash.txt",
                "sysinfo > /mnt/dell-smp-01-sysinfo.txt", "disk list -c > /mnt/dell-smp-02-disks.txt",
                "dmesg > /mnt/dell-smp-03-boot.txt", "lockstat -c > /mnt/dell-smp-04-locks-before.txt",
                "smpbench -w cpu_scale -r 5 -c > /mnt/dell-smp-05-cpu-reference.txt"]
    trials = []
    for index, workload in enumerate(("signals", "spawn_wait", "pipes"), 10):
        commands.append(f"smpbench -w {workload} -n 1 -r 3 -c > /mnt/dell-smp-{index}-one-{workload}.txt")
    index = 20
    for workload in ("signals", "spawn_wait", "pipes"):
        for mode in ("plain-1", "profile-1", "profile-2", "plain-2"):
            profile = mode.startswith("profile")
            filename = f"dell-smp-{index}-{workload}-{mode}.txt"
            command = f"smpbench -w {workload} -r 5 -c{' -p' if profile else ''} > /mnt/{filename}"
            commands.append(command)
            trials.append({"order": index, "workload": workload, "profile": profile, "file": filename, "command": command})
            index += 1
    commands.append("lockstat -c > /mnt/dell-smp-90-locks-after.txt")
    commands += ["cat /mnt/dell-smp-* | grep 'w='", "sync"]
    (out / "commands.txt").write_text("\n".join(commands) + "\n")
    (out / "SHA256SUMS").write_text("".join(f"{value['sha256']}  {name}\n" for name, value in artifacts.items()))
    (out / "git-status.txt").write_text(subprocess.check_output(["git", "status", "--short"], cwd=REPO, text=True))
    manifest = {"status": "prepared; Dell execution pending", "artifacts": artifacts, "data_partuuid": data_uuid,
                "git_head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip(),
                "source_smpbench_sha256": digest(REPO / "user/tools/smpbench.c"), "trials": trials,
                "physical_device_written": False, "verification": "GPT/filesystems plus exact embedded kernel/initramfs/tool and ISO kernel hashes"}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (out / "README.md").write_text(f"""# Dell SMP environment check

Image: `fortress-dell-smpbench.img` (130 MiB), SHA-256
`{artifacts['fortress-dell-smpbench.img']['sha256']}`.
Data partition PARTUUID: `{data_uuid}`. Symbols: `fortress.elf`.
The exact embedded benchmark hash is in `SHA256SUMS` and `manifest.json`.
Physical execution is pending. Preparation writes only regular workspace files.

1. Flash this image to your chosen disposable test USB using your usual imaging
   tool, then boot that USB on the Dell. This bundle does not flash any device.
2. Select **Persistent Storage** for the selected USB PARTUUID. This existing
   entry opts that USB into RW so full results can be saved there.
   The internal NVMe exclusion and all raw-write test gates remain unchanged.
   Check `disk list -c`: `/mnt` must be the selected USB data partition in RW mode.
   The generated first entry is RW; Recovery is RO. Use Recovery if collecting
   solely through serial/screenshots instead of saving to `/mnt`.
3. Use AC power, leave network/background jobs idle and note model, CPU,
   firmware mode, online CPU count, power settings and any thermal changes.
4. Enter the lines in `commands.txt` sequentially, one shell prompt at a time.
   The shell does not execute this text file as a script. Results go directly
   to separate `/mnt` files. `/tmp` has a 4096-byte per-file limit and cannot
   hold full benchmark logs. The last lines display summaries and sync the USB.
   If using RO, omit redirection and capture terminal/serial output instead.
5. Check all summaries for `ok=1 short=0`; profile runs should identify rev=3
   and plain runs rev=2. Check `/bin/smpbench` SHA-256 against the manifest.
   Default workers are capped at online CPUs: compare actual `n=`, not an
   assumed eight. `-n 1` is one worker with all CPUs online, not an SMP=1 boot.
6. Stop on panic, failed checksum or failure to regain the prompt. Retain the
   full exception/boot output (serial if available; otherwise photograph it).
   QEMU's frozen RAM capture does not apply to the Dell. A command taking
   longer than two minutes is a hang candidate: capture the display and report
   it before attempting more workloads. This is an observation bound, not a
   new kernel deadline.
7. After successful sync, use `shutdown`, then retrieve all `dell-smp-*.txt`
   files from the USB data partition on a host that can read ext2. Keep full
   logs, not just medians. Combine files on the host if desired.

## Minimum first pass

If entering the full off/on/on/off sequence is inconvenient, first run
`sysinfo`, `disk list -c` and these six commands visibly:

```text
smpbench -w signals -r 5 -c
smpbench -w signals -r 5 -c -p
smpbench -w spawn_wait -r 5 -c
smpbench -w spawn_wait -r 5 -c -p
smpbench -w pipes -r 5 -c
smpbench -w pipes -r 5 -c -p
```

Retain full output/photographs. The full command file adds the CPU reference,
single-worker references, repeated opposite ordering and durable collection.

## Interpretation

This is an environment check, not an old/new kernel A/B comparison. Plain and
profile mode use the same binary; timestamps and larger output still perturb
profile runs. Keep their throughput separate. Off/on/on/off ordering limits
simple order effects but does not eliminate thermal or scheduler variability.
USB output adds collection cost and can affect subsequent cohorts; parent
collection timing includes these writes. Worker timing excludes parent result
printing. For performance confirmation, also use terminal/serial capture and
keep that capture mode consistent across comparisons.
Collect profiles using the existing host analyzer:

```sh
python3 scripts/analyze_smp_profile.py /path/to/dell-smp-results.txt \\
  --output build/new-dell-phase-analysis.json
```

If large SMP latency and the same dominant phases persist on Dell, investigate
those kernel paths there. If Dell is stable while QEMU has long slow tails,
use Dell for optimization effect measurements and QEMU for correctness/capture.
If profile mode changes the distribution substantially, use it only to locate
candidate phases and confirm effects with plain trials. A mixed/insufficient
result remains inconclusive; no fixed percentage threshold is assumed.
""")
    print(f"Prepared verified Dell bundle: {out}")
    print(f"Image SHA256: {artifacts['fortress-dell-smpbench.img']['sha256']}")


if __name__ == "__main__":
    main()
