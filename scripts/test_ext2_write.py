#!/usr/bin/env python3
"""Phase 9D — Writable ext2 Filesystem Integration, Persistence, and Failure Test.

Acceptance:
1. Multi-firmware testing: legacy BIOS and UEFI with paired OVMF 4M firmware.
2. Create and write files to disposable NVMe GPT image via Ring 3 shell/editor.
3. Verify platform NVMe flush and clean ACPI S5 shutdown (QEMU exit code 0).
4. Validate offline filesystem integrity using host e2fsck -fn (0 errors).
5. Reboot on persisted image, verify exact multi-line contents.
6. Truncate and overwrite with shorter content via editor, verify block reclamation.
7. Re-audit offline filesystem integrity via e2fsck -fn (0 errors).
8. Reboot a third time and verify exact persistence of truncated state.
"""
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import threading

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))
from test_nmi_transitions import connect


def run_session(img_path, commands, log_path, mode="bios"):
    with tempfile.TemporaryDirectory(prefix="fortress-ext2-uart-") as tmp:
        uart_path = Path(tmp) / "uart"
        qemu_cmd = [
            "qemu-system-x86_64", "-M", "q35", "-m", "2G", "-display", "none",
            "-no-reboot", "-monitor", "none", "-boot", "d", "-cdrom", "bin/fortress.iso",
            "-chardev", f"socket,id=uart,path={uart_path},server=on,wait=off,logfile={log_path}",
            "-serial", "chardev:uart",
            "-drive", f"file={img_path},if=none,id=nvm0,format=raw,snapshot=off",
            "-device", "nvme,serial=fortress0,drive=nvm0"
        ]
        if mode == "uefi":
            code = Path("/usr/share/OVMF/OVMF_CODE_4M.fd")
            source = Path("/usr/share/OVMF/OVMF_VARS_4M.fd")
            if not code.exists() or not source.exists():
                raise SystemExit("UEFI verification requires paired OVMF 4M firmware")
            vars_file = Path(tmp) / "vars.fd"
            shutil.copyfile(source, vars_file)
            qemu_cmd += [
                "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={code}",
                "-drive", f"if=pflash,format=raw,unit=1,file={vars_file}"
            ]

        child = subprocess.Popen(qemu_cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            uart = connect(uart_path)
            uart.settimeout(0.2)
            stop_drain = threading.Event()
            def drain():
                while not stop_drain.is_set():
                    try:
                        if not uart.recv(65536): return
                    except socket.timeout:
                        continue
            drain_thread = threading.Thread(target=drain, daemon=True)
            drain_thread.start()

            def output():
                if not log_path.exists(): return ""
                return log_path.read_text(errors="replace").replace("\r", "")

            def wait_for(pattern, timeout=45):
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    txt = output()
                    if pattern in txt:
                        time.sleep(0.05)
                        return txt
                    assert child.poll() is None, child.stderr.read().decode()
                    time.sleep(0.05)
                raise AssertionError(f"Timeout waiting for {pattern!r} in serial log:\n{output()[-1500:]}")

            def send_str(s):
                for b in s.encode():
                    uart.send(bytes([b]))
                    time.sleep(0.005)

            # Wait for initial shell prompt
            wait_for("fortress> ")

            for cmd, expect in commands:
                send_str(cmd)
                if expect:
                    wait_for(expect)

            # Wait for clean shutdown and full QEMU process exit
            exit_code = child.wait(timeout=15)
            assert exit_code == 0, f"QEMU exited with code {exit_code}"
            stop_drain.set()
            return output()
        finally:
            if child.poll() is None:
                child.kill()
                child.wait()


def check_e2fsck(img_path):
    with tempfile.TemporaryDirectory(prefix="fortress-e2fsck-") as tmp:
        part_file = Path(tmp) / "part1.ext2"
        # Partition 1 starts at sector 2048 (1 MiB) with 8192 sectors (4 MiB)
        part_offset = 2048 * 512
        part_size = 8192 * 512
        with open(img_path, "rb") as f:
            f.seek(part_offset)
            data = f.read(part_size)
            assert len(data) == part_size, "Truncated partition data"
            part_file.write_bytes(data)

        # Run e2fsck -fn on the extracted partition
        res = subprocess.run(["e2fsck", "-fn", str(part_file)], capture_output=True, text=True)
        print("      - e2fsck output:\n" + "\n".join("        " + l for l in res.stdout.strip().splitlines()))
        assert res.returncode == 0, f"e2fsck reported filesystem errors (code {res.returncode}):\n{res.stdout}\n{res.stderr}"


def run_firmware_test(orig_img, build_dir, mode):
    test_img = build_dir / f"nvme_write_test_{mode}.img"
    print(f"\n=======================================================", flush=True)
    print(f"[TEST 9D] Starting {mode.upper()} Writable ext2 Persistence Suite", flush=True)
    print(f"=======================================================", flush=True)
    print(f"[TEST 9D] Cloning fresh fixture {orig_img} -> {test_img}...", flush=True)
    shutil.copyfile(orig_img, test_img)

    # -------------------------------------------------------------
    # Boot 1: Create and write /mnt/written.txt via shell & editor
    # -------------------------------------------------------------
    log1 = build_dir / f"ext2_write_{mode}_boot1.log"
    if log1.exists(): log1.unlink()

    print(f"[TEST 9D] [{mode.upper()}] Boot 1: Creating and writing /mnt/written.txt...", flush=True)
    boot1_commands = [
        ("ls /mnt\n", "fortress> "),
        ("edit /mnt/written.txt\n", "edit> "),
        ("a\n", "> "),
        (f"Phase 9D writable ext2 persistence test {mode} line 1\n", "> "),
        ("Second line written by Ring 3 editor\n", "> "),
        (".\n", "edit> "),
        ("stats\n", "edit> "),
        ("w\n", "Saved"),
        ("q\n", "fortress> "),
        ("cat /mnt/written.txt\n", "Second line written by Ring 3 editor"),
        ("ls /mnt\n", "written.txt"),
        ("shutdown\n", None)
    ]

    out1 = run_session(test_img, boot1_commands, log1, mode=mode)
    assert "[EDIT] Saved" in out1, f"Save confirmation missing in log:\n{out1[-1000:]}"
    assert "written.txt" in out1, f"written.txt missing from directory list:\n{out1[-1000:]}"
    print(f"      - [{mode.upper()}] Boot 1 successful: file created, edited, saved, and shutdown clean.")

    # Audit 1: Host e2fsck after clean shutdown
    print(f"[TEST 9D] [{mode.upper()}] Checking post-boot-1 integrity with host e2fsck -fn...", flush=True)
    check_e2fsck(test_img)
    print(f"      - [{mode.upper()}] Post-boot-1 e2fsck verified clean filesystem with zero errors.")

    # -------------------------------------------------------------
    # Boot 2: Verify persistence, then overwrite with shorter content (truncation)
    # -------------------------------------------------------------
    log2 = build_dir / f"ext2_write_{mode}_boot2.log"
    if log2.exists(): log2.unlink()
    print(f"[TEST 9D] [{mode.upper()}] Boot 2: Verifying persistence & testing overwrite truncation...", flush=True)
    boot2_commands = [
        ("ls /mnt\n", "written.txt"),
        ("cat /mnt/written.txt\n", "Second line written by Ring 3 editor"),
        ("edit /mnt/written.txt\n", "edit> "),
        ("d 1\n", "Deleted line 1"),
        ("d 1\n", "Deleted line 1"),
        ("a\n", "> "),
        ("Phase 9D shorter truncated single line\n", "> "),
        (".\n", "edit> "),
        ("w\n", "Saved"),
        ("q\n", "fortress> "),
        ("cat /mnt/written.txt\n", "Phase 9D shorter truncated single line"),
        ("shutdown\n", None)
    ]

    out2 = run_session(test_img, boot2_commands, log2, mode=mode)
    assert f"Phase 9D writable ext2 persistence test {mode} line 1" in out2, f"Boot 1 text missing in boot 2:\n{out2[-1000:]}"
    assert "Phase 9D shorter truncated single line" in out2, f"Truncated text missing:\n{out2[-1000:]}"
    print(f"      - [{mode.upper()}] Boot 2 successful: verified previous persistence, truncated file, and shutdown clean.")

    # Audit 2: Host e2fsck after truncation
    print(f"[TEST 9D] [{mode.upper()}] Checking post-truncation integrity with host e2fsck -fn...", flush=True)
    check_e2fsck(test_img)
    print(f"      - [{mode.upper()}] Post-truncation e2fsck verified clean filesystem with zero errors.")

    # -------------------------------------------------------------
    # Boot 3: Verify persisted truncated content across reboot
    # -------------------------------------------------------------
    log3 = build_dir / f"ext2_write_{mode}_boot3.log"
    if log3.exists(): log3.unlink()
    print(f"[TEST 9D] [{mode.upper()}] Boot 3: Verifying cross-boot persistence of truncated state...", flush=True)
    boot3_commands = [
        ("cat /mnt/written.txt\n", "Phase 9D shorter truncated single line"),
        ("shutdown\n", None)
    ]

    out3 = run_session(test_img, boot3_commands, log3, mode=mode)
    assert "Phase 9D shorter truncated single line" in out3, f"Truncated text missing in boot 3:\n{out3[-1000:]}"
    assert "Second line written by Ring 3 editor" not in out3, f"Stale old line persisted despite truncation:\n{out3[-1000:]}"
    print(f"      - [{mode.upper()}] Boot 3 successful: verified truncated content persisted with no stale lines.")


def main():
    build_dir = REPO / "build"
    build_dir.mkdir(exist_ok=True)
    orig_img = build_dir / "nvme_gpt.img"
    if not orig_img.exists():
        subprocess.run(["make", "nvme-gpt-disk"], cwd=REPO, check=True)

    # Run in both BIOS and UEFI firmware modes
    for mode in ("bios", "uefi"):
        run_firmware_test(orig_img, build_dir, mode)

    print("\n=======================================================", flush=True)
    print("PASS: Phase 9D Writable ext2 Suite (BIOS + UEFI, writes, e2fsck, truncation, cross-boot persistence).", flush=True)
    print("=======================================================\n", flush=True)


if __name__ == "__main__":
    main()
