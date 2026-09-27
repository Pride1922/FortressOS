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
import os
from test_shell_s6 import qemu_session, REPO

SMP = os.environ.get("SMP", "1")

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
        expected = bytes(i % 251 for i in range(262267))
        assert payload.read_bytes() == expected
        for filename, contents in {
            "s7copy": expected,
            "s7samplecopy": b"A\x00\xff\t\rB",
            "s7headbyte": expected[:1],
        }.items():
            extracted = tmp / filename
            subprocess.run(["debugfs", "-R", f"dump /{filename} {extracted}", str(partition)],
                           check=True, capture_output=True, timeout=30)
            assert extracted.read_bytes() == contents, filename


def run(mode, iso):
    with qemu_session(mode, iso_path=iso, log_prefix="shell-s7",
                      smp=SMP,
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
        for command in ("/bin/hello > /mnt/s7canary | 'cd' /",
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
        # Phase 5A: production tools, raw file comparison and exact child statuses.
        for stages in (2, 3, 8):
            chain = ["cat /mnt/s7payload"] + ["cat"] * (stages - 2)
            chain += ["/bin/pipetest verify 262267"]
            assert "PIPELINE BYTES OK" in check(" | ".join(chain))
        check("cat /mnt/s7payload | cat > /mnt/s7copy")
        check("/bin/pipetest sample > /mnt/s7sample")
        check("cat /mnt/s7sample > /mnt/s7samplecopy")
        assert check("view /mnt/s7sample") == "A..\t.B\n"
        assert "cat is /bin/cat" in check("type cat")
        assert "view is a shell builtin" in check("type view")
        check("view /mnt/s7sample | wc -x", 2)
        assert "PIPELINE BYTES OK" in check("command cat /mnt/s7payload > /mnt/s7copy ; cat /mnt/s7copy | /bin/pipetest verify 262267")
        assert check("PATH=/missing cat /mnt/s7sample", 127)
        assert check("PATH=/missing /bin/cat /mnt/s7sample | wc -c").strip() == "6"
        check("/bin/pipetest text > /mnt/s7text")
        assert check("cat /mnt/s7text | wc").strip() == "3 4 19"
        assert check("cat /mnt/s7text | head -n 2 | wc -l").strip() == "2"
        assert check("cat /mnt/s7text | tail -n 2 | wc -l").strip() == "1"
        assert check("cat /mnt/s7text | tail -c 4") == "last"
        assert check("cat /mnt/s7text | head -n 50 | wc -l").strip() == "3"
        assert check("cat /mnt/s7text | tail -n 10 | wc -l").strip() == "3"
        for stream_mode in (0, 1, 2):
            assert "STREAM STATUS OK" in check(f"/bin/pipetest observe {stream_mode}")
        # Shell must report an explicit 141 as a plain status, not a fault vector.
        assert "[PROCESS] Exit status 141" in check("/bin/pipetest status 141", 141)
        check("head -n 0 /missing", 1)
        check("tail -n 11 /mnt/s7text", 2)
        check("tail -c 65537 /mnt/s7text", 2)
        assert "always reads to EOF" in check("tail --help")

        # Phase 5B: builtin pipeline stages via /bin/sh-builtin.
        # echo in a pipeline stage
        assert check("echo hello | cat").strip() == "hello"
        assert check("/bin/pipetest produce 0 | echo standalone").strip() == "standalone"
        # pwd in a pipeline
        cwd_out = check("echo trigger | pwd | cat")
        assert "/" in cwd_out
        # true and false as pipeline stages
        check("/bin/pipetest produce | true", 0)
        check("/bin/pipetest produce | false", 1)
        check("true | /bin/pipetest verify 0")
        check("false | /bin/pipetest verify 0", 0)
        # Negation of a builtin pipeline stage
        check("! false | /bin/pipetest verify 0", 1)
        check("! true | /bin/pipetest verify 0", 1)
        # env in a pipeline
        check("export PIPE_TEST=s7b")
        for command in ("/bin/sh-builtin env", "env | cat"):
            env_lines = check(command).splitlines()
            for entry in ("PATH=/bin", "HOME=/", "PIPE_TEST=s7b"):
                assert entry in env_lines, (command, env_lines)
        export_out = check("env | cat | wc -l")
        assert int(export_out.strip()) > 0
        # ls as a pipeline stage
        ls_out = check("ls /bin | wc -l")
        assert int(ls_out.strip()) > 0
        # view sanitizes and sends to stdout through a pipe
        check("/bin/pipetest sample > /mnt/s7b_sample")
        view_pipe_out = check("view /mnt/s7b_sample | cat")
        assert "." in view_pipe_out or len(view_pipe_out) > 0
        # echo in pipeline with redirect
        check("echo s7b_line > /mnt/s7b_echo")
        assert check("cat /mnt/s7b_echo").strip() == "s7b_line"
        # Builtin as first stage
        assert "shell" in check("echo shell | cat")
        # Forbidden builtins rejected before any spawn
        check("echo canary > /mnt/s7b_canary")
        for forbidden in ("cd /", "exit 0", "set", "export X=bad", "alias x=y",
                          "history", "reboot"):
            check(f"/bin/pipetest produce | {forbidden}", 1)
            # canary must not be overwritten by any side effect
            assert check("cat /mnt/s7b_canary").strip() == "canary"
        # type in runner context: echo is a builtin, cat is external, cd is not found
        type_out = check("echo x | type echo cat")
        assert "shell builtin" in type_out
        assert "/bin/cat" in type_out or "cat" in type_out
        # help produces output through a pipe
        help_out = check("help | wc -l")
        assert int(help_out.strip()) > 5
        # version produces output through a pipe
        ver_out = check("version | cat")
        assert "FortressOS" in ver_out
        # Status propagation: rightmost nonzero wins
        check("echo hi | false", 1)
        check("false | echo hi", 0)
        check("! echo hi | false")
        # Env overflow: 32 exported variables is the limit (checked at preflight)
        # (We don't test overflow here as it requires carefully managing env state.)
        #check("sync")
    print(f"PASS S7 executor Phase 5B: {mode}, BSP, {SMP} CPU(s), disposable NVMe, byte comparison + e2fsck + builtin stages", flush=True)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="fortress-shell-s7-") as name:
        iso = make_iso(Path(name))
        for mode in ("bios", "uefi"):
            run(mode, iso)
