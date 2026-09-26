#!/usr/bin/env python3
"""S6 resource/failure audit against real Ring 3 spawn syscalls and ext2.

Read-only debugger observations of the unmodified kernel. Hardware breakpoints
cover the spawn interval, not just its endpoints. QEMU all-stop snapshots compare
the full PMM allocation bitmap, live heap bytes/blocks, VMM registry/tables,
descriptor references, scheduler membership, child records and stack slots.
This is deterministic lifecycle evidence, not a shared-offset concurrency proof.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import shutil
import socket
import subprocess
import tarfile
import tempfile
import threading
import time

from test_nmi_transitions import REPO, Remote, QMP, connect, symbols

CODE = Path("/usr/share/OVMF/OVMF_CODE_4M.fd")
VARS = Path("/usr/share/OVMF/OVMF_VARS_4M.fd")


def layouts(temp):
    # Compile only a host layout printer. Including thread.c exposes its private
    # scheduler type; --gc-sections discards ALL kernel functions/data. No copied
    # struct layouts, guest calls, or guest memory writes are needed.
    fields = {
        "tcb": ("tcb_t", ["tid", "next", "cr3", "stack_slot", "fd_table", "fd_flags"]),
        "file": ("file_t", ["ref_count", "node"]),
        "space": ("vmm_space_t", ["cr3", "state", "owner_refs", "sched_refs", "op_refs",
                                   "active_cpus_mask", "next"]),
        "cpu": ("cpu_local_t", ["current_thread"]),
        "sched": ("struct scheduler_cpu", ["runqueue_head", "blocked_threads", "dead_threads",
                                           "zombie_thread", "stack_slots_bitmap", "child_records"]),
        "record": ("child_record_t", ["parent", "pid", "used", "done"]),
    }
    expressions = {}
    for prefix, (ctype, members) in fields.items():
        expressions[prefix + "_size"] = f"sizeof({ctype})"
        for member in members:
            expressions[prefix + "_" + member] = f"offsetof({ctype}, {member})"
    source = temp / "layout.c"
    lines = ['#include "thread.c"', 'extern int printf(const char *, ...);', 'int main(void) {']
    for key, expr in expressions.items():
        lines.append(f'printf("{key} %zu\\n", (size_t)({expr}));')
    source.write_text("\n".join(lines + ["return 0; }"]))
    exe = temp / "layout"
    includes = ["include", "drivers", "arch/x86_64", "mm", "kernel", "lib", "fs"]
    subprocess.run(["gcc", "-std=c11", "-ffreestanding", "-fno-pie", "-no-pie",
                    "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                    *[f"-Isrc/{x}" for x in includes], str(source), "-o", str(exe)],
                   cwd=REPO, check=True, timeout=30)
    return {key: int(value) for key, value in
            (line.split() for line in subprocess.check_output([str(exe)], text=True).splitlines())}


def make_iso(temp):
    root = temp / "iso"
    shutil.copytree(REPO / "build/iso_root", root)
    initramfs = temp / "initramfs.tar"
    with tarfile.open(REPO / "bin/initramfs.tar") as original, \
            tarfile.open(initramfs, "w", format=tarfile.USTAR_FORMAT) as out:
        for member in original.getmembers():
            out.addfile(member, original.extractfile(member) if member.isfile() else None)
        out.add(REPO / "build/s6_resources.elf", arcname="bin/s6_resources")
    shutil.copyfile(initramfs, root / "boot/initramfs.tar")
    shutil.copyfile(initramfs, root / "initramfs.tar")
    config = ("timeout: 0\n/FortressOS S6 resource test\n    protocol: limine\n"
              "    kernel_path: boot():/boot/fortress.elf\n"
              "    module_path: boot():/boot/initramfs.tar\n")
    for path in (root / "limine.conf", root / "boot/limine.conf", root / "boot/limine/limine.conf"):
        path.write_text(config)
    iso = temp / "s6-resources.iso"
    subprocess.run(["xorriso", "-as", "mkisofs", "-b", "boot/limine/limine-bios-cd.bin",
                    "-no-emul-boot", "-boot-load-size", "4", "-boot-info-table",
                    "--efi-boot", "boot/limine/limine-uefi-cd.bin", "-efi-boot-part",
                    "--efi-boot-image", "--protective-msdos-label", str(root), "-o", str(iso)],
                   cwd=REPO, check=True, timeout=60, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    subprocess.run([str(REPO / "limine/limine"), "bios-install", str(iso)],
                   check=True, timeout=30, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return iso


def validate_argv(cmd, *, iso, disk, uart, qmp, gdb, log, cpus, variables=None):
    # Exact allowlist for the FINAL argv: rejects all injected arguments,
    # alternate data backends, devices and firmware overrides.
    expected = ["qemu-system-x86_64", "-accel", "tcg", "-M", "q35", "-m", "2G",
                "-smp", str(cpus), "-display", "none", "-no-reboot", "-S", "-monitor", "none",
                "-boot", "d", "-cdrom", str(iso),
                "-chardev", f"socket,id=uart,path={uart},server=on,wait=off,logfile={log}",
                "-serial", "chardev:uart", "-qmp", f"unix:{qmp},server=on,wait=off",
                "-gdb", f"unix:{gdb},server=on,wait=off",
                "-drive", f"file={disk},if=none,id=nvm0,format=raw,snapshot=off",
                "-device", "nvme,serial=fortress0,drive=nvm0",
                "-fw_cfg", "name=opt/fortress/write_test,string=1"]
    if variables is not None:
        expected += ["-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={CODE}",
                     "-drive", f"if=pflash,format=raw,unit=1,file={variables}"]
    assert cmd == expected, "Unexpected QEMU argv: only disposable NVMe, test ISO and paired firmware allowed"
    assert disk.resolve() != (REPO / "build/nvme_gpt.img").resolve()
    assert disk.parent.resolve() == iso.parent.resolve(), "Data disk must be in the disposable test directory"
    if variables is not None:
        assert variables.parent.resolve() == disk.parent.resolve()


class Audit:
    def __init__(self, remote, sym, layout, cpus):
        self.r, self.s, self.l, self.cpus = remote, sym, layout, cpus

    def number(self, address, size=8):
        return int.from_bytes(self.r.memory(address, size), "little")

    def field(self, address, name, size=8):
        return self.number(address + self.l[name], size)

    def global_value(self, name):
        return self.number(self.s[name])

    def current(self):
        return self.field(self.s["cpu_locals"], "cpu_current_thread")

    def descriptors(self, tcb):
        return [self.field(tcb + fd * 8, "tcb_fd_table") for fd in range(32)]

    def refs(self, handles):
        return {p: self.field(p, "file_ref_count", 4) for p in set(handles) if p}

    def spaces(self):
        records = {}
        p = self.global_value("g_vmm_spaces_list")
        seen = set()
        while p:
            assert p not in seen and len(seen) < 128, "Corrupt/unbounded VMM registry"
            seen.add(p)
            cr3 = self.field(p, "space_cr3")
            records[cr3] = [self.field(p, "space_" + name, 4)
                            for name in ("state", "owner_refs", "sched_refs", "op_refs")]
            p = self.field(p, "space_next")
        return records

    def assert_unpublished_space(self, cr3):
        assert self.spaces()[cr3] == [0, 1, 0, 0], "Child must be registered but unscheduled"
        p = self.global_value("g_vmm_spaces_list")
        for _ in range(128):
            assert p, "Child space missing"
            if self.field(p, "space_cr3") == cr3:
                assert self.field(p, "space_active_cpus_mask") == 0, "Child CR3 became active"
                return
            p = self.field(p, "space_next")
        raise AssertionError("Unbounded VMM registry")

    def membership(self):
        reachable = set()
        records = []
        for cpu in range(self.cpus):
            base = self.s["scheduler_cpus"] + cpu * self.l["sched_size"]
            current = self.field(self.s["cpu_locals"] + cpu * self.l["cpu_size"], "cpu_current_thread")
            if current:
                reachable.add(current)
            for field in ("runqueue_head", "blocked_threads", "dead_threads", "zombie_thread"):
                p = self.field(base, "sched_" + field)
                seen = set()
                while p:
                    assert p not in seen and len(seen) < 64, "Corrupt/unbounded scheduler list"
                    seen.add(p)
                    reachable.add(p)
                    p = self.field(p, "tcb_next") if field != "zombie_thread" else 0
            for i in range(64):
                rec = base + self.l["sched_child_records"] + i * self.l["record_size"]
                if self.field(rec, "record_used", 1):
                    records.append((cpu, i, self.field(rec, "record_parent"),
                                    self.field(rec, "record_pid"), self.field(rec, "record_done", 1)))
        return sorted(reachable), records

    def snapshot(self, parent):
        assert self.global_value("g_vmm_deferred_list") == 0, "Deferred VMM destruction not drained"
        for cpu in range(self.cpus):
            base = self.s["scheduler_cpus"] + cpu * self.l["sched_size"]
            assert self.field(base, "sched_dead_threads") == 0, "Dead threads not reaped"
            assert self.field(base, "sched_zombie_thread") == 0, "Switch handoff not finished"
        size = (self.global_value("total_pages") + 7) // 8
        assert 0 < size <= 1024 * 1024
        ptr = self.global_value("bitmap")
        bitmap = b"".join(self.r.memory(ptr + i, min(1024, size - i)) for i in range(0, size, 1024))
        handles = self.descriptors(parent)
        membership, records = self.membership()
        return dict(bitmap=bitmap, free=self.global_value("free_pages"),
                    heap_live=self.global_value("g_allocated_bytes"),
                    heap_blocks=self.global_value("g_allocated_blocks"),
                    heap_capacity=self.global_value("g_heap_end") - self.global_value("g_heap_start"),
                    tables=self.global_value("g_vmm_allocated_table_frames"), spaces=self.spaces(),
                    slots=self.field(self.s["scheduler_cpus"], "sched_stack_slots_bitmap"),
                    handles=handles, refs=self.refs(handles), membership=membership, records=records)

    def trace_spawn(self, failure):
        r, s = self.r, self.s
        r.resume_to(s["process_spawn_from_vfs_ext"])
        return_pc = self.number(r.registers()[0][7])
        parent = self.current()
        parent_handles = self.descriptors(parent)
        parent_refs = self.refs(parent_handles)
        assert parent_handles[3] and parent_handles[3] == parent_handles[4]
        assert parent_refs[parent_handles[3]] == 2
        hooks = {s[name]: name for name in ("fd_clone_table", "fd_close_all",
                                            "vmm_space_add_sched_ref", "vmm_space_enter")}
        hooks[return_pc] = "return"
        for address in hooks:
            r.breakpoint(address)
        child = cr3 = slot = None
        evidence = dict(sched_ref_calls=0, clone_observed=False, cleanup_observed=False)
        try:
            for _ in range(16):
                reply = r.request("c")
                assert reply.startswith(("T05", "S05")), reply
                regs = r.registers()[0]
                pc = regs[16]
                assert pc in hooks, f"Unexpected stop {pc:#x}"
                event = hooks.pop(pc)
                r.breakpoint(pc, False)
                if event == "fd_clone_table":
                    assert regs[5] == parent
                    child = regs[4]
                    cr3 = self.field(child, "tcb_cr3")
                    slot = self.field(child, "tcb_stack_slot", 4)
                    assert slot < 64
                    self.assert_unpublished_space(cr3)
                    evidence["registered_before_actions"] = True
                    assert child not in self.membership()[0]
                    callback = self.number(regs[7])
                    hooks[callback] = "cloned"
                    r.breakpoint(callback)
                elif event == "cloned":
                    child_handles = self.descriptors(child)
                    assert child_handles[3] == child_handles[4] == parent_handles[3]
                    counts = Counter(p for p in child_handles if p)
                    for p, refs in parent_refs.items():
                        assert self.field(p, "file_ref_count", 4) == refs + counts[p]
                    evidence["fixture_refs_before"] = parent_refs[parent_handles[3]]
                    evidence["fixture_refs_after_clone"] = self.field(parent_handles[3], "file_ref_count", 4)
                    evidence["clone_observed"] = True
                elif event == "fd_close_all":
                    assert failure and regs[5] == child, "Unexpected child cleanup before spawn return"
                    handles = self.descriptors(child)
                    assert handles[5] and handles[5] not in parent_refs, "OPEN action was not reached"
                    assert handles[6] == handles[3] == parent_handles[3] and handles[4] == 0
                    assert self.field(handles[5], "file_ref_count", 4) == 1
                    counts = Counter(p for p in handles if p)
                    for p, refs in parent_refs.items():
                        assert self.field(p, "file_ref_count", 4) == refs + counts[p]
                    assert child not in self.membership()[0]
                    self.assert_unpublished_space(cr3)
                    evidence["fixture_refs_before_cleanup"] = self.field(parent_handles[3], "file_ref_count", 4)
                    evidence["cleanup_observed"] = True
                elif event == "vmm_space_add_sched_ref":
                    assert not failure, "Failed child attempted scheduler-reference acquisition/publication"
                    assert regs[5] == cr3
                    evidence["sched_ref_calls"] += 1
                elif event == "vmm_space_enter":
                    raise AssertionError("Address space entered before local IRQ-excluded spawn returned")
                elif event == "return":
                    result = regs[0] if regs[0] < (1 << 63) else regs[0] - (1 << 64)
                    assert evidence["clone_observed"] and child is not None
                    if failure:
                        assert result == failure, (result, failure)
                        assert evidence["cleanup_observed"] and evidence["sched_ref_calls"] == 0
                        assert child not in self.membership()[0]
                        assert cr3 not in self.spaces()
                        assert self.refs(parent_handles) == parent_refs
                        assert not self.field(s["scheduler_cpus"], "sched_stack_slots_bitmap") & (1 << slot)
                    else:
                        assert result == 0 and evidence["sched_ref_calls"] == 1
                        assert child in self.membership()[0]
                        assert self.spaces()[cr3][2] == 1
                    return dict(evidence, child=child, cr3=cr3, slot=slot, result=result)
            raise AssertionError("Spawn trace exceeded bounded stop count")
        finally:
            for address in hooks:
                r.breakpoint(address, False)


def run(iso, temp, layout, firmware, cpus):
    sym = symbols()
    helper = subprocess.check_output(["nm", "-an", "build/s6_resources.elf"], cwd=REPO, text=True)
    checkpoint = int(next(line.split()[0] for line in helper.splitlines()
                          if line.endswith(" s6_resource_checkpoint")), 16)
    name = f"shell-s6-resources-{firmware}-{cpus}cpu"
    log = REPO / "build" / (name + ".log")
    log.write_text("")
    disk = temp / "nvme.img"
    shutil.copyfile(REPO / "build/nvme_gpt.img", disk)
    uart_path, qmp_path, gdb_path = [temp / x for x in ("uart", "qmp", "gdb")]
    variables = temp / "vars.fd" if firmware == "uefi" else None
    if variables:
        shutil.copyfile(VARS, variables)
    cmd = ["qemu-system-x86_64", "-accel", "tcg", "-M", "q35", "-m", "2G",
           "-smp", str(cpus), "-display", "none", "-no-reboot", "-S", "-monitor", "none",
           "-boot", "d", "-cdrom", str(iso),
           "-chardev", f"socket,id=uart,path={uart_path},server=on,wait=off,logfile={log}",
           "-serial", "chardev:uart", "-qmp", f"unix:{qmp_path},server=on,wait=off",
           "-gdb", f"unix:{gdb_path},server=on,wait=off",
           "-drive", f"file={disk},if=none,id=nvm0,format=raw,snapshot=off",
           "-device", "nvme,serial=fortress0,drive=nvm0",
           "-fw_cfg", "name=opt/fortress/write_test,string=1"]
    if variables:
        cmd += ["-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={CODE}",
                "-drive", f"if=pflash,format=raw,unit=1,file={variables}"]
    validation = dict(iso=iso, disk=disk, uart=uart_path, qmp=qmp_path, gdb=gdb_path,
                      log=log, cpus=cpus, variables=variables)
    validate_argv(cmd, **validation)
    # Negative preflight checks execute before QEMU on every firmware variant.
    for extra in (["-drive", "file=/dev/sda"], ["-blockdev", "driver=file,filename=extra.img"],
                  ["-hda", "extra.img"], ["-device", "nvme,drive=other"], ["-readconfig", "extra.cfg"]):
        try:
            validate_argv(cmd + extra, **validation)
        except AssertionError:
            pass
        else:
            raise AssertionError("Preflight accepted injected arguments")
    report = []
    stop = threading.Event()
    with (REPO / "build" / (name + ".stderr")).open("wb") as err:
        child = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=err)
        try:
            uart = connect(uart_path)
            uart.settimeout(0.2)
            def drain():
                while not stop.is_set():
                    try:
                        if not uart.recv(65536):
                            return
                    except socket.timeout:
                        pass
            reader = threading.Thread(target=drain, daemon=True)
            reader.start()
            qmp, remote = QMP(qmp_path), Remote(gdb_path)
            remote.request("qSupported")
            qmp.execute("cont")
            def wait_text(marker, start=0):
                deadline = time.monotonic() + 60
                while time.monotonic() < deadline:
                    output = log.read_text(errors="replace").replace("\r", "")
                    assert "S6 RESOURCE USER FAIL" not in output and "[FAIL]" not in output, output[-3000:]
                    if marker in output[start:]:
                        return output
                    assert child.poll() is None, f"QEMU exited: {log}"
                    time.sleep(0.05)
                raise AssertionError(f"Timed out waiting for {marker}: {log}")
            wait_text("fortress> ")
            qmp.execute("stop")
            audit = Audit(remote, sym, layout, cpus)
            def send():
                for byte in b"run /bin/s6_resources\n":
                    uart.send(bytes([byte]))
                    time.sleep(0.005)
            sender = threading.Thread(target=send, daemon=True)
            sender.start()
            remote.resume_to(checkpoint)
            sender.join(timeout=5)
            assert not sender.is_alive()
            parent = audit.current()
            slots = []
            for cycle in range(16):
                regs = remote.registers()[0]
                assert (regs[5], regs[4]) == (0, cycle), "Missing begin checkpoint"
                before = audit.snapshot(parent)
                failure = {1: -3, 2: -5}.get(cycle % 4, 0)
                trace = audit.trace_spawn(failure)
                slots.append(trace["slot"])
                remote.resume_to(checkpoint)
                regs = remote.registers()[0]
                assert (regs[5], regs[4]) == (1, cycle), "Missing end checkpoint"
                after = audit.snapshot(parent)
                # Warm-up permits retained allocator/cache growth, never fd/stack leaks.
                for key in ("handles", "refs", "slots", "spaces", "membership", "records"):
                    assert before[key] == after[key], f"{key} mismatch in cycle {cycle}"
                if cycle >= 4:
                    for key in before:
                        assert before[key] == after[key], f"{key} baseline mismatch in cycle {cycle}"
                def summary(snapshot):
                    return {**{k: v for k, v in snapshot.items() if k != "bitmap"},
                            "bitmap_sha256": hashlib.sha256(snapshot["bitmap"]).hexdigest()}
                report.append(dict(cycle=cycle, measured=cycle >= 4, trace=trace,
                                   before=summary(before), after=summary(after)))
                print(f"PASS {name} cycle={cycle} case={cycle % 4} slot={trace['slot']}"
                      f" {'baseline equality' if cycle >= 4 else 'warm-up'}", flush=True)
                assert remote.request("s").startswith(("T05", "S05"))
                if cycle != 15:
                    remote.resume_to(checkpoint)
            assert len(set(slots)) == 1, f"Expected actual stack index reuse: {slots}"
            qmp.execute("cont")
            wait_text("S6 RESOURCE USER PASS")
            wait_text("fortress> ", log.read_text(errors="replace").find("S6 RESOURCE USER PASS"))
            uart.sendall(b"poweroff\n")
            assert child.wait(timeout=20) == 0
            from test_smp_append import check_e2fsck
            check_e2fsck(disk)
            (REPO / "build" / (name + ".json")).write_text(json.dumps(report, indent=2) + "\n")
            print(f"PASS {name}: 12 measured cycles, no failed-child publication, exact cleanup,"
                  " refcount balance, slot reuse, clean e2fsck", flush=True)
        finally:
            stop.set()
            if child.poll() is None:
                child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait(timeout=5)
            for obj in (locals().get("uart"), getattr(locals().get("remote"), "sock", None),
                        getattr(locals().get("qmp"), "sock", None)):
                if obj:
                    obj.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--firmware", nargs="+", choices=("bios", "uefi"), default=["bios", "uefi"])
    parser.add_argument("--cpus", nargs="+", type=int, choices=(1, 4), default=[1, 4])
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="fortress-s6-resources-") as directory:
        temp = Path(directory)
        layout = layouts(temp)
        iso = make_iso(temp)
        for firmware in args.firmware:
            for cpus in args.cpus:
                run(iso, temp, layout, firmware, cpus)


if __name__ == "__main__":
    main()
