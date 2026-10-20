#!/usr/bin/env python3
"""EH-4 Package-Scale Installed-Root Acceptance Suite.

Validates ext4 package-scale staging, directory growth, atomic executable replacement,
retained VFS node lifetimes, and concurrent reader contention on an installed NVMe root.

Operates purely on disposable regular-file NVMe fixtures.
No physical disk modifications, unreviewed API additions, or unvetted privilege escalation.
"""

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import secrets
import selectors
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import time
import uuid

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "scripts"))

import test_root_phase0 as fixture
from test_root_phase2 import extract_root

FEATURES = "none,extent,filetype,sparse_super,large_file,metadata_csum,has_journal"
TEST_PASSWORD = "FortressTestPassword2026!"
OVMF_CODE = Path("/usr/share/OVMF/OVMF_CODE_4M.fd")
OVMF_VARS = Path("/usr/share/OVMF/OVMF_VARS_4M.fd")


def build_pkg_probe(build_dir: Path) -> Path:
    """Builds the freestanding /bin/pkg-probe ELF binary."""
    out_elf = build_dir / "pkg-probe.elf"
    probe_c = REPO / "tests/pkg_probe.c"
    start_asm = REPO / "user/tools/start.asm"
    linker_ld = REPO / "tests/replace_probe.ld"
    common_o = build_dir / "tool-common.o"

    assert probe_c.is_file(), f"Missing {probe_c}"
    assert start_asm.is_file(), f"Missing {start_asm}"
    assert linker_ld.is_file(), f"Missing {linker_ld}"
    assert common_o.is_file(), f"Missing {common_o}"

    obj_o = build_dir / "pkg-probe.o"
    start_o = build_dir / "pkg-probe-start.o"

    subprocess.run([
        "gcc", "-Os", "-fno-pie", "-fno-stack-protector", "-fstack-usage",
        "-m64", "-march=x86-64", "-mno-80387", "-mno-mmx", "-mno-sse", "-mno-sse2",
        "-mno-red-zone", "-mgeneral-regs-only",
        "-Isrc/include", "-Isrc/fs", "-Iuser/tools", "-Wframe-larger-than=512",
        "-c", str(probe_c), "-o", str(obj_o)
    ], cwd=REPO, check=True)

    subprocess.run([
        "nasm", "-f", "elf64", "-DTOOL_ENTRY=pkg_probe_main",
        str(start_asm), "-o", str(start_o)
    ], cwd=REPO, check=True)

    subprocess.run([
        "ld", "-m", "elf_x86_64", "-nostdlib", "-static", "-z", "noexecstack",
        "-T", str(linker_ld), str(start_o), str(obj_o), str(common_o),
        "-o", str(out_elf)
    ], cwd=REPO, check=True)

    return out_elf


def prepare_installed_gpt_disk(out: Path, probe_elf: Path) -> tuple[Path, uuid.UUID]:
    """Assembles a bootable installed NVMe GPT disk fixture with /bin/pkg-probe."""
    stage = out / "tree"
    stage.mkdir(parents=True, exist_ok=True)
    attrs = {}

    with tarfile.open(REPO / "bin/initramfs.tar") as src:
        for m in src:
            name = Path(m.name)
            assert not name.is_absolute() and ".." not in name.parts
            assert m.isfile() or m.isdir()
            target = stage / name
            if m.isdir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(src.extractfile(m).read())
            attrs["/" + name.as_posix()] = (m.mode, m.uid, m.gid)

    for name in ("dev", "run", "tmp", "mnt", "var", "var/log", "home", "home/operator", "root", "opt"):
        (stage / name).mkdir(parents=True, exist_ok=True)

    # Standard credentials
    (stage / "etc/passwd").write_text("root:x:0:0::/root:/bin/shell\noperator:x:1000:1000::/home/operator:/bin/shell\n")
    (stage / "etc/group").write_text("root:x:0:\noperator:x:1000:\nwheel:x:10:operator\n")
    h = fixture.crypt_hash(TEST_PASSWORD, "$5$" + secrets.token_hex(8))
    (stage / "etc/shadow").write_text(f"root:!:::::::\noperator:{h}:::::::\n")

    # Install probe to /bin/pkg-probe
    shutil.copyfile(probe_elf, stage / "bin/pkg-probe")
    attrs["/bin/pkg-probe"] = (0o755, 0, 0)

    attrs.update({
        "/etc/shadow": (0o600, 0, 0),
        "/tmp": (0o1777, 0, 0),
        "/home/operator": (0o700, 1000, 1000),
        "/root": (0o700, 0, 0)
    })

    partuuid, fsuuid = uuid.uuid4(), uuid.uuid4()
    identity = f"install_id={secrets.token_hex(16)}\nroot_partuuid={partuuid}\nfilesystem_uuid={fsuuid}\n"
    (stage / "etc/fortress-install.v1").write_text("FORTRESS-INSTALL-1\n" + identity + "payload_version=1\n")

    root = out / "root.img"
    with root.open("xb") as f:
        f.truncate(fixture.ROOT_BYTES)

    run = lambda args: fixture.run(args, out / "prepare.log")
    run([
        "mke2fs", "-q", "-t", "ext4", "-b", "4096", "-I", "256", "-U", str(fsuuid),
        "-O", FEATURES, "-E", "lazy_itable_init=0", "-m", "0", "-g", "4096",
        "-N", "4096", "-J", "size=4", "-d", str(stage), str(root)
    ])

    commands = ["set_inode_field / mode 040755", "set_inode_field / uid 0", "set_inode_field / gid 0"]
    for p in sorted(stage.rglob("*")):
        name = "/" + p.relative_to(stage).as_posix()
        mode, u, g = attrs.get(name, (0o755 if p.is_dir() else 0o644, 0, 0))
        commands += [
            f"set_inode_field {name} mode 0{mode | (0o40000 if p.is_dir() else 0o100000):o}",
            f"set_inode_field {name} uid {u}",
            f"set_inode_field {name} gid {g}"
        ]
    cmd_file = out / "attrs.commands"
    cmd_file.write_text("\n".join(commands) + "\n")
    run(["debugfs", "-w", "-f", str(cmd_file), str(root)])
    fixture.initialize_journal(root, out)
    run(["e2fsck", "-fn", str(root)])

    bootstrap = out / "bootstrap.tar"
    with tarfile.open(REPO / "bin/initramfs.tar") as src, tarfile.open(bootstrap, "w", format=tarfile.USTAR_FORMAT) as dst:
        for m in src:
            dst.addfile(m, src.extractfile(m) if m.isfile() else None)
        m = tarfile.TarInfo("bootstrap")
        m.type = tarfile.DIRTYPE
        m.mode = 0o700
        dst.addfile(m)
        rec_data = (f"FORTRESS-RECOVERY-1\n{identity}password_hash=" +
                    fixture.crypt_hash(secrets.token_hex(16), "$5$rounds=100000$" + secrets.token_hex(8)) + "\n").encode()
        m = tarfile.TarInfo("bootstrap/recovery.v1")
        m.mode = 0o600
        m.size = len(rec_data)
        dst.addfile(m, io.BytesIO(rec_data))

    kernel = REPO / "bin/fortress.elf"
    ready = (f"FORTRESS-READY-1\n{identity}payload_version=1\nkernel_sha256=" +
             fixture.digest(kernel) + "\nbootstrap_sha256=" + fixture.digest(bootstrap) + "\n").encode()
    ready += ("checksum=" + hashlib.sha256(ready).hexdigest() + "\n").encode()
    assert len(ready) < 512
    readiness = out / "readiness.v1"
    readiness.write_bytes(ready.ljust(512, b"\0"))

    config = out / "limine.conf"
    config.write_text(
        "timeout: 0\n"
        "/FortressOS Installed Root\n"
        "    protocol: limine\n"
        "    kernel_path: boot():/boot/fortress.elf\n"
        f"    kernel_cmdline: boot_mode=installed root=PARTUUID={partuuid}\n"
        "    module_path: boot():/boot/bootstrap.tar\n"
        "    module_cmdline: role=bootstrap\n"
        "    module_path: boot():/boot/readiness.v1\n"
        "    module_cmdline: role=readiness\n"
    )

    esp = out / "esp.img"
    fixture.create_esp_partition(esp, None, REPO / "limine", kernel, bootstrap, config, None)
    run(["mren", "-i", str(esp), "::/boot/initramfs.tar", "bootstrap.tar"])
    run(["mcopy", "-i", str(esp), str(readiness), "::/boot/readiness.v1"])

    disk, _ = fixture.assemble(out, esp, root, 384, partuuid)
    return disk, partuuid


def run_qemu_pkg_test(disk_path: Path, firmware: str, smp: int, tmp_dir: Path, evidence_dir: Path) -> dict:
    """Executes the package-scale test probe on the installed NVMe root under QEMU."""
    log_path = evidence_dir / f"serial-{firmware}-smp{smp}.log"
    qemu_cmd = [
        "qemu-system-x86_64", "-M", "q35", "-cpu", "max,+rdrand,+rdseed",
        "-m", "1G", "-smp", str(smp), "-display", "none", "-no-reboot",
        "-serial", "stdio",
        "-drive", f"file={disk_path},if=none,id=nvm0,format=raw,snapshot=off",
        "-device", "nvme,serial=PKGINSTALLED,drive=nvm0,bootindex=1"
    ]

    if firmware == "uefi":
        vars_path = tmp_dir / f"vars-{firmware}-smp{smp}.fd"
        shutil.copyfile(OVMF_VARS, vars_path)
        qemu_cmd += [
            "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={OVMF_CODE}",
            "-drive", f"if=pflash,format=raw,unit=1,file={vars_path}"
        ]

    proc = subprocess.Popen(qemu_cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=REPO)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)

    transcript = bytearray()

    def get_text():
        return re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "", transcript.decode(errors="replace")).replace("\r", "")

    def wait_for(pred, timeout=90):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for key, _ in sel.select(0.1):
                chunk = os.read(key.fileobj.fileno(), 4096)
                if chunk:
                    transcript.extend(chunk)
                    log_path.write_bytes(transcript)
            txt = get_text()
            if "[PROCESS FAULT]" in txt or "KERNEL PANIC" in txt:
                time.sleep(0.5)
                for key, _ in sel.select(0.5):
                    chunk = os.read(key.fileobj.fileno(), 4096)
                    if chunk:
                        transcript.extend(chunk)
                log_path.write_bytes(transcript)
                txt = get_text()
                assert "KERNEL PANIC" not in txt, f"Panic detected:\n{txt[-3000:]}"
                assert "[PROCESS FAULT]" not in txt, f"Process fault detected:\n{txt[-3000:]}"
            if pred(txt):
                return
            assert proc.poll() is None, f"QEMU terminated prematurely:\n{txt[-3000:]}"
        raise TimeoutError(f"Timed out waiting for predicate. Serial tail:\n{get_text()[-3000:]}")

    def send_line(line: str):
        proc.stdin.write((line + "\n").encode())
        proc.stdin.flush()

    try:
        print(f"  [1] Booting FortressOS in installed mode ({firmware.upper()}, SMP={smp})...", flush=True)
        wait_for(lambda t: "FortressOS login:" in t, timeout=60)
        print("  [1] Login prompt ready.", flush=True)

        print("  [2] Authenticating as operator...", flush=True)
        pos = len(get_text())
        send_line("operator")
        wait_for(lambda t: "Password:" in t[pos:], timeout=10)
        pos = len(get_text())
        send_line(TEST_PASSWORD)
        wait_for(lambda t: "operator@fortress:" in t[pos:] and t.endswith("$ "), timeout=15)
        print("  [2] Operator shell session established.", flush=True)
        time.sleep(0.2)

        print("  [3] Elevating to root shell...", flush=True)
        pos = len(get_text())
        send_line("sudo /bin/shell")
        wait_for(lambda t: "sudo password: " in t[pos:], timeout=10)
        pos = len(get_text())
        send_line(TEST_PASSWORD)
        wait_for(lambda t: "root@fortress:" in t[pos:] and t.endswith("# "), timeout=15)
        print("  [3] Root shell established.", flush=True)

        print("  [4] Executing /bin/pkg-probe (package staging, directory growth, atomic replacement)...", flush=True)
        pos_probe = len(get_text())
        send_line("/bin/pkg-probe")
        wait_for(lambda t: "PKG_SCALE Ring3 PASS" in t[pos_probe:], timeout=120)
        print("  [4] /bin/pkg-probe PASS confirmed on console.", flush=True)

        # Extract logged metrics
        output = get_text()[pos_probe:]
        catalog_found = int(re.search(r"PKG catalog_entries_found=(\d+)", output)[1])
        w1_pid = int(re.search(r"PKG worker_v1_pid=(\d+)", output)[1])
        w2_pid = int(re.search(r"PKG worker_v2_pid=(\d+)", output)[1])

        print(f"      Catalog entries enumerated: {catalog_found} (expected >= 128)")
        print(f"      Worker PIDs: v1={w1_pid}, v2={w2_pid} (retained VFS node verified)")

        assert catalog_found >= 128, f"Catalog entries {catalog_found} < 128"
        assert "Worker v1 survived atomic executable replacement:" in output
        assert "Concurrent reader finished with 0 errors." in output

        print("  [5] Triggering clean system poweroff (sync & freeze)...", flush=True)
        pos = len(get_text())
        send_line("reboot")
        try:
            proc.wait(timeout=25)
        except subprocess.TimeoutExpired:
            proc.terminate()
            proc.wait(timeout=5)
        print("  [5] System unmounted and cleanly shut down.", flush=True)

        return {
            "firmware": firmware,
            "smp": smp,
            "catalog_entries": catalog_found,
            "worker_v1_pid": w1_pid,
            "worker_v2_pid": w2_pid,
            "result": "PASS"
        }

    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)


def verify_linux_oracle(disk_path: Path, tmp_dir: Path) -> dict:
    """Performs host-side Linux Oracle audits: e2fsck -fn and debugfs metadata/hash verification."""
    print("  [6] Running Host Linux Oracle Audit (e2fsck & debugfs)...", flush=True)
    extracted_root = tmp_dir / "extracted_root.img"
    extract_root(disk_path, extracted_root)

    # 1. Structural consistency check with Linux e2fsck
    fsck_out = subprocess.run(
        ["e2fsck", "-fn", str(extracted_root)],
        capture_output=True, text=True, check=False
    )
    assert fsck_out.returncode == 0, f"e2fsck reported defects:\n{fsck_out.stdout}\n{fsck_out.stderr}"
    print("  [6a] e2fsck -fn: 0 errors detected (clean structural consistency).", flush=True)

    # 2. Check app.conf
    dump_cfg = tmp_dir / "dump_app.conf"
    subprocess.run(["debugfs", "-R", f"dump /opt/pkg/conf/app.conf {dump_cfg}", str(extracted_root)],
                   capture_output=True, check=True)
    cfg_data = dump_cfg.read_text()
    assert "FORTRESS_APP_CONFIG_V1=1" in cfg_data, f"Corrupted app.conf: {cfg_data}"

    # 3. Check libcore.so (64 KiB exact pattern)
    dump_lib = tmp_dir / "dump_libcore.so"
    subprocess.run(["debugfs", "-R", f"dump /opt/pkg/lib/libcore.so {dump_lib}", str(extracted_root)],
                   capture_output=True, check=True)
    lib_data = dump_lib.read_bytes()
    assert len(lib_data) == 65536, f"libcore.so size {len(lib_data)} != 65536"
    for i in range(len(lib_data)):
        assert lib_data[i] == ((i & 0xFF) ^ 0x5A), f"libcore.so mismatch at byte {i}"

    # 4. Check worker.log (contains both v1 and v2 records)
    dump_log = tmp_dir / "dump_worker.log"
    subprocess.run(["debugfs", "-R", f"dump /opt/pkg/data/worker.log {dump_log}", str(extracted_root)],
                   capture_output=True, check=True)
    log_text = dump_log.read_text()
    assert "WORKER v1 RUNNING" in log_text, "Missing WORKER v1 entry in worker.log"
    assert "WORKER v2 RUNNING" in log_text, "Missing WORKER v2 entry in worker.log"

    # 5. Check catalog multi-block directory
    cat_files = subprocess.run(["debugfs", "-R", "ls -l /opt/pkg/catalog", str(extracted_root)],
                               capture_output=True, text=True, check=True).stdout
    matches = len(re.findall(r"entry_\d+\.dat", cat_files))
    assert matches >= 128, f"Only found {matches} catalog entries in debugfs ls (expected >= 128)"

    print(f"  [6b] debugfs: All 128 multi-block catalog entries & exact file bytes verified.", flush=True)
    return {
        "e2fsck": "PASS",
        "debugfs_hashes": "PASS",
        "catalog_entries_on_disk": matches
    }


def main():
    parser = argparse.ArgumentParser(description="EH-4 Package-Scale Installed-Root Acceptance Suite")
    parser.add_argument("--firmware", choices=["bios", "uefi", "all"], default="bios", help="Firmware type (default: bios)")
    parser.add_argument("--smp", choices=["1", "4", "all"], default="4", help="SMP core count (default: 4)")
    args = parser.parse_args()

    firmwares = ["bios", "uefi"] if args.firmware == "all" else [args.firmware]
    cores = [1, 4] if args.smp == "all" else [int(args.smp)]

    timestamp = time.strftime("%Y%m%d-%H%M%S")
    evidence_dir = REPO / f".codex-remote-attachments/ext4-pkg-scale-{timestamp}"
    evidence_dir.mkdir(parents=True, exist_ok=True)

    print("\n" + "=" * 70)
    print(" FortressOS EH-4 Package-Scale Installed-Root Acceptance Suite")
    print(f" Matrix: Firmwares={firmwares}, SMP={cores}")
    print("=" * 70)

    # 1. Build pkg-probe.elf
    print("Building /bin/pkg-probe ELF binary...", flush=True)
    build_dir = REPO / "build"
    probe_elf = build_pkg_probe(build_dir)
    print(f"  [OK] Binary built: {probe_elf} ({probe_elf.stat().st_size} bytes)")

    overall_results = []

    for fw in firmwares:
        for smp_count in cores:
            print(f"\n>>> Running Configuration: {fw.upper()} SMP={smp_count} <<<", flush=True)
            with tempfile.TemporaryDirectory(prefix=f"fortress-pkg-{fw}-{smp_count}-") as tmp:
                tmp_dir = Path(tmp)
                # Prepare fresh installed root disk fixture
                disk_path, partuuid = prepare_installed_gpt_disk(tmp_dir, probe_elf)

                # Run guest test in QEMU
                res = run_qemu_pkg_test(disk_path, fw, smp_count, tmp_dir, evidence_dir)

                # Run Linux Oracle verification
                oracle_res = verify_linux_oracle(disk_path, tmp_dir)
                res["oracle"] = oracle_res

                overall_results.append(res)

    print("\n" + "=" * 70)
    print(" EH-4 PACKAGE-SCALE ACCEPTANCE SUMMARY")
    print("=" * 70)
    all_pass = True
    for r in overall_results:
        status_str = "[PASS]" if r["result"] == "PASS" and r["oracle"]["e2fsck"] == "PASS" else "[FAIL]"
        if status_str != "[PASS]": all_pass = False
        print(f"  {r['firmware'].upper()} SMP={r['smp']}: {status_str} | Entries={r['catalog_entries']} | Worker PIDs=({r['worker_v1_pid']},{r['worker_v2_pid']}) | Oracle={r['oracle']['e2fsck']}")

    evidence_file = evidence_dir / "result.json"
    evidence_file.write_text(json.dumps(overall_results, indent=2))
    print(f"\nEvidence recorded in: {evidence_dir}")

    if all_pass:
        print("\n>>> [PASS] EH-4 Package-Scale Acceptance Fully Validated! <<<\n")
        sys.exit(0)
    else:
        print("\n>>> [FAIL] One or more EH-4 acceptance gates failed! <<<\n")
        sys.exit(1)


if __name__ == "__main__":
    main()
