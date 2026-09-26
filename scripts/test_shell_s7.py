#!/usr/bin/env python3
"""BSP-only Phase 4 acceptance: disposable ISO/NVMe copy, BIOS and paired UEFI.

Real >256 KiB blocking transfers and byte verification, ordering, grouping,
cooperative failure recovery. Host orchestration separately injects allocation,
fd and wait failures. This runner does not claim exact kernel allocator balance.
"""
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
from test_shell_s6 import qemu_session, REPO


def make_iso(tmp):
    root = tmp / "iso"
    shutil.copytree(REPO / "build/iso_root", root)
    archive = tmp / "initramfs.tar"
    with tarfile.open(REPO / "bin/initramfs.tar") as src, \
            tarfile.open(archive, "w", format=tarfile.USTAR_FORMAT) as dst:
        for member in src.getmembers():
            dst.addfile(member, src.extractfile(member) if member.isfile() else None)
        dst.add(REPO / "build/pipeline_fixture.elf", arcname="bin/pipetest")
    for path in (root / "boot/initramfs.tar", root / "initramfs.tar"):
        shutil.copyfile(archive, path)
    config = ("timeout: 0\n/FortressOS S7 Executor Test\n    protocol: limine\n"
              "    kernel_path: boot():/boot/fortress.elf\n"
              "    module_path: boot():/boot/initramfs.tar\n")
    for path in (root / "limine.conf", root / "boot/limine.conf", root / "boot/limine/limine.conf"):
        path.write_text(config)
    iso = tmp / "s7.iso"
    subprocess.run(["xorriso", "-as", "mkisofs", "-b", "boot/limine/limine-bios-cd.bin",
                    "-no-emul-boot", "-boot-load-size", "4", "-boot-info-table",
                    "--efi-boot", "boot/limine/limine-uefi-cd.bin", "-efi-boot-part",
                    "--efi-boot-image", "--protective-msdos-label", str(root), "-o", str(iso)],
                   check=True, timeout=60, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run([str(REPO / "limine/limine"), "bios-install", str(iso)],
                   check=True, timeout=30, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return iso


def audit_disk(disk, mode):
    with tempfile.TemporaryDirectory(prefix="fortress-s7-audit-") as tmp_name:
        tmp = Path(tmp_name)
        partition = tmp / "ext2.img"
        with disk.open("rb") as src:
            src.seek(2048 * 512)  # Geometry of the disposable nvme_gpt fixture.
            data = src.read(8192 * 512)
        assert len(data) == 8192 * 512
        partition.write_bytes(data)
        result = subprocess.run(["e2fsck", "-fn", str(partition)], capture_output=True,
                                text=True, timeout=30)
        (REPO / "build" / f"shell-s7-{mode}-e2fsck.log").write_text(result.stdout + result.stderr)
        assert result.returncode == 0, result.stdout + result.stderr
        payload = tmp / "payload"
        subprocess.run(["debugfs", "-R", f"dump /s7payload {payload}", str(partition)],
                       check=True, capture_output=True, timeout=30)
        # >256 KiB, but below this 1 KiB ext2 fixture's single-indirect limit.
        assert payload.read_bytes() == bytes(i % 251 for i in range(262267))


def run(mode, iso):
    with qemu_session(mode, iso_path=iso, log_prefix="shell-s7",
                      disk_audit=lambda disk: audit_disk(disk, mode)) as (body, _, __):
        def check(command, status=0):
            out = body(command)
            assert body("echo $?").strip() == str(status), (command, out)
            assert "Faulted" not in out and "PIPE USER FAIL" not in out, out
            return out

        for stages in (2, 3, 8):
            chain = ["/bin/pipetest produce"] + ["/bin/pipetest relay"] * (stages - 2)
            chain += ["/bin/pipetest verify"]
            assert "PIPELINE BYTES OK" in check(" | ".join(chain))
        check("/bin/pipetest produce 262267 | /bin/pipetest relay > /mnt/s7payload")
        assert "PIPELINE BYTES OK" in check("/bin/pipetest relay < /mnt/s7payload | /bin/pipetest verify 262267")
        check("/bin/pipetest produce | /bin/pipetest early")
        check("/bin/pipetest status 9 | /bin/pipetest status 7", 7)
        check("! /bin/pipetest status 9 | /bin/pipetest status 7")
        check("! /bin/pipetest status 9 | /bin/pipetest status 0", 1)
        assert "AFTER" in check("false && /bin/pipetest produce | echo bad ; echo AFTER")
        assert "AFTER" in check("true || echo bad | /bin/pipetest relay ; echo AFTER")
        assert "RECOVER" in check("/bin/pipetest status 0 | /bin/pipetest status 7 && echo BAD || echo RECOVER")
        check("/bin/pipetest status 0 | /bin/pipetest status 0 && /bin/pipetest produce 0 | /bin/pipetest verify 0")
        check("/bin/pipetest status 0 | /bin/pipetest status 0 ; /bin/pipetest produce 0 | /bin/pipetest verify 0")

        check("echo intact > /mnt/s7canary")
        for command in ("/bin/hello > /mnt/s7canary | 'echo' bad",
                        "/bin/hello > /mnt/s7canary | > /mnt/s7unused",
                        "/bin/hello > /mnt/s7canary | ! /bin/pipetest relay",
                        "/bin/hello > /mnt/s7canary | $S7_UNSET"):
            check(command, 1)
            assert check("cat /mnt/s7canary").strip() == "intact"
        check("/bin/hello > /mnt/s7canary | /bin/pipetest relay" + " 2>&1" * 15 +
              " | /bin/pipetest relay", 1)
        assert check("cat /mnt/s7canary").strip() == "intact"
        check("/bin/pipetest produce 0 | /bin/pipetest relay" + " 2>&1" * 14 +
              " | /bin/pipetest verify 0")
        check(" | ".join(["/bin/hello"] * 9), 2)

        # Explicit stdout redirect overrides the pipe and the reader sees EOF.
        assert "PIPELINE BYTES OK" in check("/bin/pipetest produce 262267 > /mnt/s7override | /bin/pipetest verify 0")
        out = check("/bin/dual_stream 2>&1 > /mnt/s7out | /bin/pipetest relay")
        assert "STDERR" in out.upper(), out
        out = check("/bin/dual_stream > /mnt/s7out 2>&1 | /bin/pipetest verify 0")
        assert "PIPELINE BYTES OK" in out

        # Missing first/middle/last executables: peers consume EOF or handle EPIPE.
        for command in ("/missing | /bin/pipetest relay",
                        "/bin/pipetest produce | /missing | /bin/pipetest relay",
                        "/bin/pipetest produce | /bin/pipetest relay | /missing"):
            check(command, 127)
        for _ in range(32):
            check("/bin/pipetest produce | /bin/pipetest early")
        assert "PIPELINE BYTES OK" in check("/bin/pipetest produce | /bin/pipetest verify")
        #check("sync")
    print(f"PASS S7 executor: {mode}, BSP, disposable NVMe, byte comparison + e2fsck", flush=True)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="fortress-shell-s7-") as name:
        iso = make_iso(Path(name))
        for mode in ("bios", "uefi"):
            run(mode, iso)
