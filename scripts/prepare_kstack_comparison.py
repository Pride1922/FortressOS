#!/usr/bin/env python3
"""Freeze matched stack-map or ELF-copy kernels without changing workspace outputs."""
import argparse
import difflib
import hashlib
import json
import shlex
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


def run(args):
    return subprocess.run([str(arg) for arg in args], cwd=REPO, check=True,
                          capture_output=True, text=True, timeout=120)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--experiment", choices=("kstack", "elf-copy", "resched", "resched-policy"), default="kstack")
    parser.add_argument("--pipe-waits", action="store_true", help="Prepare a pipe observer comparison with scheduler wait diagnostics.")
    args = parser.parse_args()
    assert not args.pipe_waits or args.experiment in ('elf-copy','resched','resched-policy')
    out = args.output.resolve()
    assert out.is_relative_to(REPO / "build") and out != REPO / "build"
    out.mkdir(parents=True, exist_ok=False)
    source = (REPO / "src/kernel/thread.c").read_text()
    start = source.index("    int status = allocated == STACK_USABLE_SIZE / PAGE_SIZE")
    end = source.index("    *out_guard = guard_addr;", start)
    control = '''    int status = allocated == STACK_USABLE_SIZE / PAGE_SIZE ? VMM_OK : VMM_ERR_NOMEM;
    size_t installed = 0;
    while (status == VMM_OK && installed < allocated) {
        status = vmm_map_page(pml4, base_addr + installed * PAGE_SIZE, frames[installed],
                              PTE_PRESENT | PTE_WRITABLE | PTE_NX);
        if (status == VMM_OK) installed++;
    }
    if (status != VMM_OK) {
        while (installed) {
            installed--;
            if (vmm_unmap_page(pml4, base_addr + installed * PAGE_SIZE) != VMM_OK) {
                serial_raw_puts("[FATAL] Control stack rollback failed; frames retained\\n");
                for (;;) __asm__ volatile("cli; hlt");
            }
        }
        while (allocated) pmm_free_page(frames[--allocated]);
        rflags = spin_lock_irqsave(&g_kstack_lock);
        g_stack_slots_bitmap &= ~(1ULL << slot);
        spin_unlock_irqrestore(&g_kstack_lock, rflags);
        return -1;
    }

'''
    baseline = source[:start] + control + source[end:]
    unit = "thread"
    if args.experiment == 'resched':
        baseline = '#define FORTRESS_RESCHED_RETURN_DISABLED 1\n' + source
    if args.experiment == 'resched-policy':
        baseline = '#define FORTRESS_RESCHED_ALL_WORK 1\n' + source
    if args.experiment == "elf-copy":
        unit = "elf"
        source = (REPO / "src/kernel/elf.c").read_text()
        baseline = source.replace('#include "elf_page.h"\n', '')
        start = baseline.index('            size_t copy_len = page_file_end')
        end = baseline.index('\n\n            SPAWN_ADD(profile, elf[SE_COPY]', start)
        baseline = baseline[:start] + '''            memset(frame_virt, 0, PAGE_SIZE);
            if (page_file_end > page_file_start) {
                size_t copy_len = page_file_end - page_file_start;
                size_t file_offset = p->p_offset + (page_file_start - p->p_vaddr);
                size_t page_dest_offset = page_file_start - page;
                memcpy(frame_virt + page_dest_offset, img_bytes + file_offset, copy_len);
            }''' + baseline[end:]
        baseline = baseline.replace('    elf_page_init(restorer_kvirt, sigrestorer_start, 0, restorer_stub_size);',
                                    '    memset(restorer_kvirt, 0, PAGE_SIZE);\n    memcpy(restorer_kvirt, sigrestorer_start, restorer_stub_size);')
        baseline = baseline.replace('    elf_page_init(stack_virt, img_bytes, 0, 0);', '    memset(stack_virt, 0, PAGE_SIZE);')
        assert 'elf_page_init' not in baseline
        shutil.copyfile(REPO / 'src/kernel/elf_page.h', out / 'elf_page.h')
    (out / "thread-A.c").write_text(baseline)
    (out / "thread-B.c").write_text(source)
    (out / "source-difference.patch").write_text("".join(difflib.unified_diff(
        baseline.splitlines(True), source.splitlines(True), fromfile="A/thread.c", tofile="B/thread.c")))
    compile_lines = run(["make", "-n", "-W", "src/kernel/thread.c", "build/kernel/thread.o"]).stdout.splitlines()
    compile_cmd = next(shlex.split(line) for line in compile_lines if " -c src/kernel/thread.c " in line)
    link_lines = run(["make", "-n", "-W", "build/kernel/thread.o", "bin/fortress.elf"]).stdout.splitlines()
    link_cmd = next(shlex.split(line) for line in link_lines if line.startswith("ld "))
    if unit == "elf":
        # Resolve the loader's actual compiler flags independently of thread.c.
        compile_lines = run(["make", "-n", "-W", "src/kernel/elf.c", "build/kernel/elf.o"]).stdout.splitlines()
        compile_cmd = next(shlex.split(line) for line in compile_lines if " -c src/kernel/elf.c " in line)
    source_path = f"src/kernel/{unit}.c"
    object_name = f"build/kernel/{unit}.o"
    objects = [arg for arg in link_cmd if arg.endswith(".o")]
    common = {}
    for obj in objects:
        dest = out / "objects" / obj
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(REPO / obj, dest)
        common[obj] = digest(dest)
    shutil.copyfile(REPO / "linker.ld", out / "linker.ld")
    shutil.copyfile(REPO / "bin/initramfs.tar", out / "initramfs.tar")
    shutil.copyfile(REPO / "build/tool-smpbench.elf", out / "smpbench.elf")
    with tarfile.open(out / "initramfs.tar") as archive:
        member = next(m for m in archive if m.name.removeprefix("./") == "bin/smpbench")
        assert hashlib.sha256(archive.extractfile(member).read()).hexdigest() == digest(out / "smpbench.elf")
    manifest = {"scope": "Fresh matched control A versus batch B; same preallocated data frames, all other object bytes and initramfs identical",
                "status": "prepared; hardware comparison pending", "experiment": args.experiment, "common_objects": common,
                "thread_sources": {"A": digest(out / "thread-A.c"), "B": digest(out / "thread-B.c")},
                "compiler_command_template": compile_cmd, "linker_command_template": link_cmd,
                "initramfs_sha256": digest(out / "initramfs.tar"), "smpbench_sha256": digest(out / "smpbench.elf"),
                "trial_order": ["A1", "B1", "B2", "A2"], "variants": {}}
    for variant in ("A", "B"):
        directory = out / variant
        directory.mkdir()
        object_path = directory / "thread.o"
        if variant == "A":
            command = [str(out / "thread-A.c") if arg == source_path else
                       str(object_path) if arg == object_name else arg for arg in compile_cmd]
            run(command)
        else:
            shutil.copyfile(out / "objects" / object_name, object_path)
        kernel = directory / "fortress.elf"
        command = [str(object_path) if arg == object_name else
                   str(out / "objects" / arg) if arg in objects else
                   str(out / "linker.ld") if arg == "linker.ld" else
                   str(kernel) if arg == "bin/fortress.elf" else arg for arg in link_cmd]
        run(command)
        if variant == "B":
            assert digest(kernel) == digest(REPO / "bin/fortress.elf"), "Workspace objects changed or build was stale"
        stage = directory / "iso-root"
        shutil.copytree(REPO / "build/iso_root", stage)
        shutil.copyfile(kernel, stage / "boot/fortress.elf")
        for target in (stage / "boot/initramfs.tar", stage / "initramfs.tar"):
            shutil.copyfile(out / "initramfs.tar", target)
        iso = directory / "fortress.iso"
        run(["xorriso", "-as", "mkisofs", "-b", "boot/limine/limine-bios-cd.bin", "-no-emul-boot",
             "-boot-load-size", "4", "-boot-info-table", "--efi-boot", "boot/limine/limine-uefi-cd.bin",
             "-efi-boot-part", "--efi-boot-image", "--protective-msdos-label", stage, "-o", iso])
        run([REPO / "limine/limine", "bios-install", iso])
        image = directory / f"fortress-kstack-{variant}.img"
        result = run(["python3", "scripts/create_boot_img.py", image, "--iso-root", stage,
                      "--kernel", kernel, "--initramfs", out / "initramfs.tar"])
        (directory / "image-verification.txt").write_text(result.stdout + result.stderr)
        with image.open("rb") as stream:
            stream.seek(512); header = stream.read(512)
            stream.seek(struct.unpack_from("<Q", header, 72)[0] * 512); entries = stream.read(256)
        esp = struct.unpack_from("<Q", entries, 32)[0] * 512
        partuuid = str(uuid.UUID(bytes_le=entries[144:160])).upper()
        with tempfile.TemporaryDirectory(prefix="fortress-kstack-image-check-") as temp:
            temp = Path(temp)
            for member, expected in (("fortress.elf", kernel), ("initramfs.tar", out / "initramfs.tar")):
                dest = temp / member
                run(["mcopy", "-i", f"{image}@@{esp}", f"::/boot/{member}", dest])
                assert digest(dest) == digest(expected), "Embedded raw artifact mismatch"
                dest.unlink()
                run(["xorriso", "-osirrox", "on", "-indev", iso, "-extract", "/boot/" + member, dest])
                assert digest(dest) == digest(expected), "Embedded ISO artifact mismatch"
        artifacts = {p.name: digest(p) for p in (kernel, iso, image, object_path)}
        manifest["variants"][variant] = {"artifacts": artifacts, "data_partuuid": partuuid}
        (directory / "SHA256SUMS").write_text("".join(f"{value}  {name}\n" for name, value in artifacts.items()))
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    commands = ["sha256sum /bin/smpbench > /mnt/kstack-00-toolhash.txt",
                "sysinfo > /mnt/kstack-01-sysinfo.txt", "lockstat -c > /mnt/kstack-02-locks-before.txt",
                "smpbench -w spawn_wait -n 8 -r 7 -c > /mnt/kstack-10-spawn-plain.txt",
                "smpbench -w spawn_wait -n 8 -r 5 -c -p > /mnt/kstack-11-spawn-profile.txt",
                "smpbench -w pipes -n 8 -r 5 -c > /mnt/kstack-12-pipes-plain.txt",
                "smpbench -w pipes -n 8 -r 5 -c -p > /mnt/kstack-13-pipes-profile.txt",
                "lockstat -c > /mnt/kstack-90-locks-after.txt", "sync", "shutdown"]
    (out / "commands.txt").write_text("\n".join(commands) + "\n")
    (out / "README.md").write_text("""# Dell kernel-stack comparison

A/fortress-kstack-A.img: control, four single-page mappings and shootdowns.
B/fortress-kstack-B.img: optimized, one batch mapping and range shootdown.
Both reserve all four data frames first; this A control is a fresh matched
counterfactual, not the earlier frozen image. Same benchmark/initramfs and all
other kernel objects. Exact differences and hashes are retained in this bundle.

Run order: A1, B1, B2, A2. Flash the named image to the chosen disposable test
USB, boot Persistent Storage, and enter commands.txt one prompt at a time.
Keep AC power, firmware, background work and thermal conditions consistent.
Check ok=1 short=0 in summaries. Full stdout goes to /mnt, not /tmp.
After each shutdown, export kstack-*.txt with Linux Reader into a separate
C:/Sources/FortressOS/build/kstack-dell-results/A1 (or B1, B2, A2) folder
BEFORE the next run or reflash. Reboot the same B image for B2. Reflash A for A2.
The files on the USB are overwritten each run; preserve each export.

This bundle never flashes a physical device. Image verification does not prove
a hardware performance gain. Plain throughput is the effect measurement;
profiles attribute changes but add observer cost. Full methodology and tests:
docs/roadmap/kstack-batch-map.md in the repository.
""")
    if args.experiment == "elf-copy":
        # Give this experiment accurate artifact names and instructions.
        for variant in ("A", "B"):
            directory = out / variant
            for old, new in ((f"fortress-kstack-{variant}.img", f"fortress-elf-copy-{variant}.img"), ('thread.o','elf.o')):
                (directory / old).rename(directory / new)
                manifest['variants'][variant]['artifacts'][new] = manifest['variants'][variant]['artifacts'].pop(old)
            (directory / 'SHA256SUMS').write_text(''.join(f'{value}  {name}\n' for name,value in manifest['variants'][variant]['artifacts'].items()))
        for variant in ('A','B'):
            (out / f'thread-{variant}.c').rename(out / f'elf-{variant}.c')
        manifest['elf_sources'] = manifest.pop('thread_sources')
        manifest['elf_page_header_sha256'] = digest(out / 'elf_page.h')
        manifest['scope'] = 'ELF page initialization only: scalar clear-then-copy A versus integer string copy-and-clear-complement B; both retain batch kernel stack mapping'
        (out / 'source-difference.patch').write_text(''.join(difflib.unified_diff(baseline.splitlines(True), source.splitlines(True), fromfile='A/elf.c', tofile='B/elf.c')))
        (out / 'commands.txt').write_text('\n'.join(commands).replace('kstack-', 'elf-copy-') + '\n')
        (out / 'README.md').write_text('''# Dell ELF page initialization comparison

A/fortress-elf-copy-A.img: scalar full-page clear then file-byte copy.
B/fortress-elf-copy-B.img: integer string copy, zero only the complement.
Both retain the proven kernel stack batching. No mapping/permissions/ownership
change. All other linked objects and the benchmark/initramfs are identical.

Run A1 -> B1 -> B2 -> A2 on AC power with consistent firmware/background and
thermal conditions. Boot Persistent Storage and run commands.txt one command
at a time. Preserve the BEFORE lock snapshot before starting benchmarks.
After sync/shutdown export elf-copy-*.txt to separate folders:
C:/Sources/FortressOS/build/elf-copy-dell-results/{A1,B1,B2,A2}
Export each run before the next boot/reflash overwrites USB filenames.
B2 uses the same stick/image after reboot; A2 requires reflashing A.
Plain timings measure the effect; profiles attribute elapsed costs.
No physical performance gain is established yet. No physical device is written
by this packager. Exact hashes and differences are retained here.
''')
        (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    if args.pipe_waits:
        assert b'wait_diag' in (out / 'smpbench.elf').read_bytes()
        diagnostic_sources = {}
        for relative in ('src/include/pipe_profile_abi.h','src/fs/pipe.h','src/fs/pipe.c',
                         'src/include/wait_profile_abi.h','src/kernel/wait_profile.h',
                         'src/kernel/thread.c','src/kernel/thread.h','src/kernel/syscall.c',
                         'src/include/syscall_abi.h','user/tools/smpbench.c',
                         'src/kernel/resched_return.h','src/arch/x86_64/smp.c',
                         'src/arch/x86_64/percpu.h','src/arch/x86_64/interrupts.asm',
                         'src/include/resched_policy.h','src/arch/x86_64/apic.h','src/arch/x86_64/smp.h'):
            target = out / 'diagnostics' / relative
            target.parent.mkdir(parents=True,exist_ok=True)
            shutil.copyfile(REPO / relative,target)
            diagnostic_sources[relative] = digest(target)
        manifest['diagnostic_sources'] = diagnostic_sources
        commands = ['sha256sum /bin/smpbench > /mnt/pipe-wait-00-toolhash.txt',
                    'sysinfo > /mnt/pipe-wait-01-sysinfo.txt',
                    'lockstat -c > /mnt/pipe-wait-02-locks-before.txt']
        for index, (mode, flags) in enumerate((('plain-1',''),('wait-1',' --wait-profile'),
                                               ('phase-1',' -p'),('phase-2',' -p'),
                                               ('wait-2',' --wait-profile'),('plain-2','')),10):
            commands.append(f'smpbench -w pipes -n 8 -r 7 -c{flags} > /mnt/pipe-wait-{index}-{mode}.txt')
        commands += ['lockstat -c > /mnt/pipe-wait-90-locks-after.txt', 'sync', 'shutdown']
        (out / 'commands.txt').write_text('\n'.join(commands)+'\n')
        (out / 'README.md').write_text('''# Dell pipe wait investigation

A/fortress-elf-copy-A.img has scalar ELF page initialization.
B/fortress-elf-copy-B.img has the fast ELF page initialization.
Both retain batch kernel stacks and identical scheduler wait instrumentation.
No scheduling or pipe wakeup policy change. Shared benchmark/initramfs and
all other linked objects. Sources, object snapshots and hashes retained.

Run A1 -> B1 -> B2 -> A2. Boot Persistent Storage RW, AC power, consistent
firmware/background/thermal conditions. Enter commands.txt one prompt at a time.
Each boot runs plain -> wait-only -> phases -> phases -> wait-only -> plain;
same eight workers, seven timed reps and one warmup per command. Wait-only
adds scheduler transition clocks without per-syscall phase clocks. Phases
also includes reader wait clocks and existing writer startup telemetry.
Counters cover all reader worker scheduler waits, including header/read and
waitpid; they are not per-read or writer wait telemetry. Selection is before
CR3/context switch; resume is the original scheduler sleep continuation.

After sync/shutdown export pipe-wait-*.txt into separate folders:
C:/Sources/FortressOS/build/pipe-wait-dell-results/{A1,B1,B2,A2}
Export before the next boot/reflash overwrites filenames. B2 reboots same B
stick; A2 reflashes A. Keep all reps. Plain timings remain primary evidence;
both diagnostic modes can affect timing. No hardware cause/fix is claimed yet.
This packager writes regular files only and never flashes a physical device.
''')
        manifest['purpose'] = 'Pipe regression investigation; plain/wait-only/phase observer comparison; matched slow/fast ELF variants'
        manifest['capture_order'] = ['plain-1','wait-1','phase-1','phase-2','wait-2','plain-2']
        (out / 'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    if args.experiment in ('resched','resched-policy'):
        for variant in ('A','B'):
            directory = out / variant
            old,new = f'fortress-kstack-{variant}.img', f'fortress-resched-{variant}.img'
            (directory/old).rename(directory/new)
            manifest['variants'][variant]['artifacts'][new] = manifest['variants'][variant]['artifacts'].pop(old)
            (directory/'SHA256SUMS').write_text(''.join(f'{value}  {name}\n' for name,value in manifest['variants'][variant]['artifacts'].items()))
        manifest['scope'] = 'Safe user-return reschedule service disabled in A and enabled in B; both include outgoing-context theft exclusion, pending publication/selection cleanup, IRQ return assembly, fast ELF copy, batch kernel stacks and benchmark/initramfs'
        manifest['purpose'] = 'Measure safe reschedule service and pipe runnable delay'
        commands = (out/'commands.txt').read_text().replace('pipe-wait-', 'resched-')
        commands = commands.replace('lockstat -c > /mnt/resched-90-locks-after.txt',
            'smpbench -w spawn_wait -n 8 -r 5 -c > /mnt/resched-20-spawn-plain.txt\nlockstat -c > /mnt/resched-90-locks-after.txt')
        (out/'commands.txt').write_text(commands)
        (out/'README.md').write_text('''# Dell safe reschedule comparison

A/fortress-resched-A.img: pending user-return service disabled (control).
B/fortress-resched-B.img: pending user-return service enabled (candidate).
Both publish/coalesce requests in the IPI handler and clear them on normal
task selection; both include outgoing-context theft exclusion and identical return hooks/fast ELF copy/batch stacks,
wait diagnostics and benchmark. Only thread.o's service implementation differs.
A is a fresh controlled counterfactual, not the previous diagnostic image.
Sources, object snapshots, hashes and embedded raw/ISO verification retained.

Run A1 -> B1 -> B2 -> A2 with AC power and consistent firmware/background/
thermal conditions. Boot Persistent Storage RW; enter commands.txt in order.
Per boot: pipes plain/wait-only/phase/phase/wait-only/plain (8 workers, 7 reps),
then plain spawn_wait (8 workers, 5 reps) as a throughput regression check.
All output goes to /mnt; sync/shutdown then export resched-*.txt into:
C:/Sources/FortressOS/build/resched-dell-results/{A1,B1,B2,A2}
Export before the next run/reflash overwrites files. B2 reboots same B stick;
A2 reflashes A. Plain timings measure effect; wait fields attribute delay.
No hardware benefit is established yet. This tool never flashes a device.
''')
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
        if args.experiment == 'resched-policy':
            for variant in ('A','B'):
                directory = out/variant
                old,new = f'fortress-resched-{variant}.img',f'fortress-resched-policy-{variant}.img'
                (directory/old).rename(directory/new)
                artifacts=manifest['variants'][variant]['artifacts']
                artifacts[new]=artifacts.pop(old)
                (directory/'SHA256SUMS').write_text(''.join(f'{value}  {name}\n' for name,value in artifacts.items()))
            manifest['scope']='A services urgent and fresh-work requests; B services urgent waiter/signal requests only. Common reason vectors, counters, outgoing-context guard, all other objects and benchmark/initramfs identical.'
            manifest['purpose']='Measure prompt wakeup service without immediate fresh-process preemption'
            commands=(out/'commands.txt').read_text().replace('resched-','policy-')
            (out/'commands.txt').write_text(commands)
            (out/'README.md').write_text('''# Dell reschedule reason policy comparison

A/fortress-resched-policy-A.img: immediate service for wake/signal and fresh work.
B/fortress-resched-policy-B.img: immediate wake/signal service; fresh work wakes
idle CPUs but does not force a busy user task to yield at a return boundary.
Both include identical reason IPIs/counters, outgoing-context theft exclusion,
return hooks, fast ELF initialization, batch stacks and benchmark/initramfs.
Only thread.o's service policy differs. A is a fresh matched control.

Run A1 -> B1 -> B2 -> A2 with consistent AC/firmware/background/thermal state.
Boot Persistent Storage RW and run commands.txt, then sync/shutdown and export
/mnt/policy-*.txt to C:/Sources/FortressOS/build/resched-policy-dell-results/
A1, B1, B2 or A2 before the next run/reflash overwrites the captures.
B2 reboots the same B stick; A2 reflashes A. No benefit is claimed before
physical comparison. This tool never flashes a device.
''')
            (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(f"Matched A/B bundle prepared: {out}")
    for variant, data in manifest["variants"].items():
        image_name = f"fortress-{'elf-copy' if args.experiment == 'elf-copy' else args.experiment if args.experiment in ('resched','resched-policy') else 'kstack'}-{variant}.img"
        print(f"{variant} image SHA256: {data['artifacts'][image_name]}")


if __name__ == "__main__":
    main()
