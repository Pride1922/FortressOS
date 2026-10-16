#!/usr/bin/env python3
"""Phase 0 only: reuse installed baseline on an instrumented disposable snapshot.

No production file is patched. No handoff engine is implemented here.
"""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
import http.server
import threading

import test_net_installed as baseline


def instrument(workspace):
    overrides = {}
    patches = {
        'src/kernel/syscall.c': (
            '    uint64_t syscall_nr = frame->rax;',
            '''    /* Disposable Phase 0 observation after the real Ring 3 entry stub. */
    if ((frame->cs & 3) == 3 && thread_current() &&
        !strcmp(thread_current()->name, "/bin/net-cpu-probe") &&
        (frame->rax == SYS_NETCTL || frame->rax == SYS_SOCKET)) {
        serial_puts("[NET SMP0 ENTRY] cpu="); serial_print_hex(cpu_current()->id);
        serial_puts(" nr="); serial_print_hex(frame->rax); serial_puts("\\n");
    }
    uint64_t syscall_nr = frame->rax;'''),
        'src/net/net.c': (
            '    net_poll_budget_t rx_budget={0};',
            '''    /* Disposable Phase 0 owner observation; release sources unchanged. */
    serial_puts("[NET SMP0 OWNER] cpu="); serial_print_hex(cpu_current()->id);
    serial_puts("\\n");
    net_poll_budget_t rx_budget={0};'''),
        'src/kernel/thread.c': (
            '''        process_record_commit(p->tid);
        p->state=THREAD_READY;
        uint64_t flags=spin_lock_irqsave(&g_sched_lock);
        runqueue_push_locked(p);
        spin_unlock_irqrestore(&g_sched_lock, flags);''',
            '''        process_record_commit(p->tid);
        p->state=THREAD_READY;
        /* Isolated fixture launch only: staged release otherwise enqueues on BSP. */
        if (!strcmp(p->name, "/bin/net-cpu-probe") && p->cpu_affinity > 0) {
            size_t target = (size_t)p->cpu_affinity;
            uint64_t flags=spin_lock_irqsave(&scheduler_cpus[target].sched_lock);
            runqueue_push_cpu_locked(target, p);
            spin_unlock_irqrestore(&scheduler_cpus[target].sched_lock, flags);
            smp_send_resched(target);
        } else {
            uint64_t flags=spin_lock_irqsave(&g_sched_lock);
            runqueue_push_locked(p);
            spin_unlock_irqrestore(&g_sched_lock, flags);
        }'''),
    }
    for name, (anchor, replacement) in patches.items():
        path = workspace / name
        original = path.read_bytes()
        content = original.decode().replace('\r\n', '\n')
        if name.endswith('thread.c') and anchor not in content:
            # Phase 3 production release now honors the initialized target for
            # every child. Legacy explicit-worker probes need no launch patch.
            assert 'size_t target=p->current_cpu;' in content
            assert 'runqueue_push_cpu_locked(target,p);' in content
            continue
        assert content.count(anchor) == 1, name
        if name.endswith('net.c'):
            content = '#include "percpu.h"\n' + content
        path.write_text(content.replace(anchor, replacement))
        overrides[name] = {'before': hashlib.sha256(original).hexdigest(),
                           'after': baseline.fixture.digest(path)}
    (workspace / 'network-smp0-overrides.json').write_text(json.dumps(overrides, indent=2))


def provenance(out, case, cpus):
    text = (out / (case + '.serial.log')).read_text()
    owners = re.findall(r'\[NET SMP0 OWNER\] cpu=(0x[0-9A-Fa-f]+)', text)
    entries = re.findall(r'\[NET SMP0 ENTRY\] cpu=(0x[0-9A-Fa-f]+) nr=(0x[0-9A-Fa-f]+)', text)
    assert owners and all(int(cpu, 16) == 0 for cpu in owners), owners
    observed = {(int(cpu, 16), int(nr, 16)) for cpu, nr in entries}
    assert len({nr for cpu, nr in observed if cpu == 0}) == 2, entries
    if cpus > 1:
        assert len({nr for cpu, nr in observed if cpu == 1}) == 2, entries
    assert all(cpu in {0, 1} for cpu, nr in observed), entries
    return {'owner_cpus': sorted({int(cpu, 16) for cpu in owners}),
            'ring3_entries': sorted([list(item) for item in observed])}


def main():
    parent = baseline.fixture.REPO / 'build/network-smp-phase0'
    parent.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix='gate-', dir=parent))
    workspace = Path(subprocess.check_output(
        ['python3', 'scripts/create_ext4_guest_workspace.py'], text=True).strip())
    assert workspace.resolve().is_relative_to(baseline.fixture.REPO / '.codex-remote-attachments')
    manifest = {'phase': 0, 'result': 'FAIL', 'workspace': str(workspace), 'cases': [],
                'scope': 'Current installed baseline: real AP control query and socket rejection; full handoff acceptance is Phase 2A'}
    source_manifest = json.loads((workspace / 'workspace-manifest.json').read_text())
    (out / 'sources.json').write_text(json.dumps(source_manifest, indent=2))
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2))
    print(out, flush=True)
    try:
        instrument(workspace)
        with (out / 'build.log').open('w') as log:
            subprocess.run(['make', '-j4', 'bin/fortress.elf', 'bin/initramfs.tar'], cwd=workspace,
                           stdout=log, stderr=subprocess.STDOUT, check=True, timeout=600)
    except Exception as error:
        manifest['failure'] = str(error)
        (out / 'manifest.json').write_text(json.dumps(manifest, indent=2))
        raise
    manifest['build_sha256'] = {name: baseline.fixture.digest(workspace / name)
                                for name in ('bin/fortress.elf', 'bin/initramfs.tar')}
    class Peer(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            data = b'network-socket-proof\n'
            self.send_response(200)
            self.send_header('Content-Length', str(len(data)))
            self.end_headers(); self.wfile.write(data)
        def log_message(self, *args):
            pass
    peer = http.server.HTTPServer(('127.0.0.1', 0), Peer)
    thread = threading.Thread(target=peer.serve_forever, daemon=True); thread.start()
    try:
        password = baseline.secrets.token_hex(16)
        disk = baseline.prepare(workspace, out, password)
        before = baseline.fixture.digest(disk)
        manifest['baseline_sha256'] = before
        for firmware in ('bios', 'uefi'):
            for cpus in (1, 4, 8):
                target = out / f'{firmware}-smp{cpus}.img'
                baseline.fixture.run(['cp', '--sparse=always', str(disk), str(target)], out / 'copy.log')
                for restart in (0, 1):
                    result = baseline.boot(out, target, firmware, cpus, restart, password, peer.server_port)
                    result.update(provenance(out, result['case'], cpus))
                    manifest['cases'].append(result)
        assert baseline.fixture.digest(disk) == before
        # Existing snapshot files must still match the source workspace.
        changed = [name for name, sha in source_manifest['sources'].items()
                   if baseline.fixture.digest(baseline.fixture.REPO / name) != sha]
        assert not changed, changed
        manifest['source_preservation'] = 'PASS'
        manifest['result'] = 'PASS'
    except Exception as error:
        manifest['failure'] = str(error)
        raise
    finally:
        peer.shutdown(); peer.server_close(); thread.join(timeout=10)
        (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        print(out, flush=True)
    subprocess.run(['python3', 'scripts/audit_network_smp_phase0.py', str(out)], check=True)


if __name__ == '__main__':
    main()
