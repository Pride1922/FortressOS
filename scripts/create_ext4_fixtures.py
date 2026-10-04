#!/usr/bin/env python3
"""Disposable ext4 format/read fixtures; never accepts a disk/output path."""
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FEATURES = "none,extent,filetype,sparse_super,large_file,metadata_csum"
MASKS = (0, 0x42, 0x403)


def run(argv, timeout=120):
    p = subprocess.run(argv, text=True, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, check=False, timeout=timeout)
    if p.returncode:
        raise RuntimeError(f"{argv!r}: exit {p.returncode}\n{p.stdout}")
    return p.stdout


def main():
    for tool in ("mke2fs", "dumpe2fs", "e2fsck", "debugfs"):
        if not shutil.which(tool):
            raise RuntimeError(f"missing {tool}")
    parent = ROOT / "build" / "ext4-phase0"
    parent.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="fixtures-", dir=parent))
    version = run(["mke2fs", "-V"])
    records = []
    for bs in (1024, 2048, 4096):
        image = output / f"e4a-{bs}.img"
        with image.open("xb") as f:
            f.truncate(64 * 1024 * 1024)
        uuid = f"e4000000-0000-4000-8000-{bs:012d}"
        argv = ["mke2fs", "-q", "-t", "ext4", "-b", str(bs), "-I", "256",
                "-O", FEATURES, "-E", "lazy_itable_init=0", "-m", "0",
                "-U", uuid, str(image)]
        mkfs = run(argv)
        raw = image.read_bytes()
        masks = struct.unpack_from("<III", raw, 1024 + 0x5c)
        if masks != MASKS:
            raise RuntimeError(f"profile drift: {masks!r}, expected {MASKS!r}")
        # Fill with independent Linux tooling, then verify the payload round trip.
        payload = output / f"payload-{bs}.bin"
        payload.write_bytes(bytes(range(256)) * 4096)
        populate = run(["debugfs", "-w", "-R", f"write {payload} /data.bin", str(image)])
        recovered = output / f"recovered-{bs}.bin"
        run(["debugfs", "-R", f"dump /data.bin {recovered}", str(image)])
        if recovered.read_bytes() != payload.read_bytes():
            raise RuntimeError("debugfs payload round trip failed")
        fragmented = output / f"fragmented-{bs}.bin"
        with fragmented.open("xb") as f:
            for k in range(1600):
                f.seek(k*2*bs)
                f.write(bytes([(k % 251)+1])*bs)
            f.truncate(3200*bs)
        populate += run(["debugfs", "-w", "-R", f"write {fragmented} /fragmented.bin", str(image)])
        # Linux temporary storage preserves sparseness without retaining a
        # >4GiB host file on Windows/DrvFS. The ext4 image retains the marker.
        with tempfile.TemporaryDirectory(prefix="fortress-ext4-sparse-") as tmp:
            sparse = Path(tmp) / "sparse.bin"
            with sparse.open("xb") as f:
                f.seek(2**32+bs+17)
                f.write(b"EXT4-above-4GiB")
            populate += run(["debugfs", "-w", "-R", f"write {sparse} /sparse.bin", str(image)])
        populate += run(["debugfs", "-w", "-R", "write /dev/null /unwritten.bin", str(image)])
        populate += run(["debugfs", "-w", "-R", "fallocate /unwritten.bin 0 7", str(image)])
        populate += run(["debugfs", "-w", "-R", f"set_inode_field /unwritten.bin size {8*bs}", str(image)])
        populate += run(["debugfs", "-R", "stat /fragmented.bin", str(image)])
        header = run(["dumpe2fs", "-h", str(image)])
        fsck = run(["e2fsck", "-fn", str(image)])
        inode = run(["debugfs", "-R", "stat /data.bin", str(image)])
        evidence = output / f"e4a-{bs}.txt"
        evidence.write_text(version + mkfs + populate + header + fsck + inode)
        raw = image.read_bytes()
        records.append({"image": image.name, "filesystem_block_size": bs,
                        "sector_adapters": [512, 4096], "uuid": uuid,
                        "mkfs_argv": argv, "feature_masks": list(masks),
                        "sha256": hashlib.sha256(raw).hexdigest(),
                        "payload_sha256": hashlib.sha256(payload.read_bytes()).hexdigest(),
                        "payload_size": payload.stat().st_size,
                        "fragmented_sha256": hashlib.sha256(fragmented.read_bytes()).hexdigest(),
                        "fragmented_size": fragmented.stat().st_size,
                        "sparse_marker_offset": 2**32+bs+17,
                        "unwritten_size": 8*bs,
                        "evidence": evidence.name})
        # Malformed admission vectors: checksum becomes invalid too. Phase 1
        # must separately build valid-checksum feature fixtures to test masks.
        variants = {"unknown-incompat": (0x60, 0x80000000),
                    "unknown-ro-compat": (0x64, 0x80000000),
                    "journal-unimplemented": (0x5c, 0x4),
                    "needs-recovery": (0x60, 0x4)}
        for name, (offset, bit) in variants.items():
            changed = bytearray(raw)
            at = 1024 + offset
            value = struct.unpack_from("<I", changed, at)[0]
            struct.pack_into("<I", changed, at, value | bit)
            (output / f"reject-{bs}-{name}.img").write_bytes(changed)
        corrupt = bytearray(raw)
        corrupt[1024 + 0x78] ^= 1
        (output / f"reject-{bs}-checksum.img").write_bytes(corrupt)
        print(f"PASS {bs}: exact feature masks, Linux 1MiB round trip, e2fsck clean")
    artifacts = [{"name": p.name, "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
                 for p in sorted(output.glob("*.img"))]
    (output / "manifest.json").write_text(json.dumps(
        {"tool_version": version, "profile": "E4-A-v1", "images": records,
         "artifacts": artifacts}, indent=2) + "\n")
    print(f"Evidence: {output}")


if __name__ == "__main__":
    main()
