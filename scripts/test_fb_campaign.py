#!/usr/bin/env python3
"""Isolated graphics SMP campaign. Snapshot-only observers; no data disks."""
import argparse
import hashlib
import json
import re
import subprocess
import tempfile
import tarfile
import time
from pathlib import Path
from test_compositor import ROOT, Guest
from test_fb_lifecycle import make_iso, rejected_frees
from test_nmi_transitions import QMP


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def replace_once(path, old, new):
    text = path.read_text()
    assert text.count(old) == 1, (path, old, text.count(old))
    path.write_text(text.replace(old, new))


def prepare(output):
    workspace = Path(subprocess.check_output(['python3', 'scripts/create_ext4_guest_workspace.py'], cwd=ROOT, text=True).strip())
    (output / 'workspace.txt').write_text(str(workspace))
    thread = workspace / 'src/kernel/thread.c'
    replace_once(thread, '    t->current_cpu = cpu_current()->id;\n    t->cpu_affinity = -1;\n    t->cpus_allowed = CPU_MASK_ALL;',
                 '''    t->current_cpu = cpu_current()->id;
    t->cpu_affinity = -1;
    t->cpus_allowed = CPU_MASK_ALL;
    /* Snapshot-only raw test thread placement, before publication. */
    if (!strcmp(curr->name,"/bin/fb-smp-probe")) {
        if (arg>=g_total_sched_cpus) __builtin_trap();
        t->current_cpu=arg; t->cpu_affinity=(int)arg; t->cpus_allowed=CPU_MASK_ONE(arg);
    }''')
    with (workspace / 'src/mm/pmm.c').open('a') as f:
        f.write('''
/* Snapshot-only ownership observer, under the actual PMM lock. */
bool fb_campaign_root_allocated(uintptr_t root) {
    uint64_t flags=spin_lock_irqsave(&g_pmm_lock);
    size_t index=root/PAGE_SIZE;
    bool allocated=(root%PAGE_SIZE)!=0 || index>=total_pages || bitmap_test(index);
    spin_unlock_irqrestore(&g_pmm_lock,flags);
    return allocated;
}
''')
    replace_once(thread, 'static void thread_group_forget_joins(uintptr_t cr3);',
                 'static void thread_group_forget_joins(uintptr_t cr3);\nextern void fb_campaign_reap_begin(uintptr_t);\nextern void fb_campaign_reap_end(uintptr_t);')
    replace_once(thread, '            if (destroy_space) {\n',
                 '            if (destroy_space) {\n                uintptr_t observed_root=dead->cr3;\n                fb_campaign_reap_begin(observed_root);\n')
    replace_once(thread, '                kfree(dead);\n            } else if (!keep_leader)',
                 '                kfree(dead);\n                fb_campaign_reap_end(observed_root);\n            } else if (!keep_leader)')
    syscall = workspace / 'src/kernel/syscall.c'
    replace_once(syscall, '#include "fb.h"', '#include "fb.h"\nextern int64_t fb_campaign_dispatch(uint64_t,uintptr_t,uint64_t);')
    replace_once(syscall, '    switch (syscall_nr) {', '    switch (syscall_nr) {\n        case 0x7ff5:\n            result=fb_campaign_dispatch(frame->rdi,frame->rsi,frame->rdx);\n            break;')
    fb = workspace / 'src/kernel/fb.c'
    replace_once(fb, '#include "fb.h"', '#include "fb.h"\nextern void fb_campaign_release_begin(uintptr_t);\nextern void fb_campaign_release_end(void);')
    anchor = '    int error=mapped_bytes ? vmm_unmap_device_range(mapped_root,mapped_va,mapped_pa,mapped_bytes) : VMM_OK;'
    replace_once(fb, anchor, '    fb_campaign_release_begin(cr3);\n'+anchor+'\n    fb_campaign_release_end();')
    idt = workspace / 'src/arch/x86_64/idt.c'
    replace_once(idt, '#include "crash_dump.h"', '#include "crash_dump.h"\nextern void fb_campaign_note_fault(interrupt_frame_t *);')
    replace_once(idt, '        unsigned sig = crash_dump_vector_to_signal(frame->vector);',
                 '        fb_campaign_note_fault(frame);\n        unsigned sig = crash_dump_vector_to_signal(frame->vector);')
    smp = workspace / 'src/arch/x86_64/smp.c'
    replace_once(smp, 'static void smp_ipi_tlb_handler(interrupt_frame_t *frame) {\n    (void)frame;',
                 'extern void fb_campaign_note_ipi(interrupt_frame_t *);\nstatic void smp_ipi_tlb_handler(interrupt_frame_t *frame) {\n    fb_campaign_note_ipi(frame);')
    (workspace / 'src/kernel/fb_campaign_observer.c').write_text((ROOT / 'tests/fb_campaign_observer.c').read_text())
    # Header is available only in this private kernel tree.
    (workspace / 'src/kernel/fb_campaign.h').write_text((ROOT / 'tests/fb_campaign.h').read_text())
    with (workspace / 'Makefile').open('a') as f:
        f.write('''
build/fb-smp-probe.elf: tests/fb_smp_probe.c tests/fb_campaign.h build/tool-common.o
\t@$(CC) $(CFLAGS) -Os -fno-pie -fno-asynchronous-unwind-tables -fstack-usage -Wframe-larger-than=1024 -Iuser -Iuser/tools -Itests -c tests/fb_smp_probe.c -o build/fb-smp-probe.o
\t@$(AS) -f elf64 -DTOOL_ENTRY=fb_smp_probe_main user/tools/start.asm -o build/fb-smp-start.o
\t@$(LD) -nostdlib -static -z noexecstack -T user/shell.ld build/fb-smp-start.o build/fb-smp-probe.o build/tool-common.o -o $@
''')
    with (output / 'build.log').open('wb') as log:
        subprocess.run(['make', '-j4', 'bin/fortress.iso', 'build/fb-smp-probe.elf'], cwd=workspace,
                       stdout=log, stderr=subprocess.STDOUT, check=True, timeout=900)
    return workspace


def boot(workspace, output, firmware, modes):
    with tempfile.TemporaryDirectory(prefix='fortress-fb-campaign-') as directory:
        tmp = Path(directory)
        iso = make_iso(tmp, workspace, {'fb-smp-probe': workspace / 'build/fb-smp-probe.elf'})
        kernel = tmp / 'iso/boot/fortress.elf'
        guest = Guest(firmware, 4, tmp, iso)
        evidence = {'firmware': firmware, 'smp': 4, 'kernel_sha256': digest(kernel), 'probe_sha256': digest(workspace / 'build/fb-smp-probe.elf'), 'initramfs_sha256': digest(tmp / 'initramfs.tar'), 'modes': []}
        try:
            guest.wait(lambda s: s.endswith('FortressOS login: '), timeout=60)
            guest.qmp = QMP(tmp / 'qmp.sock')
            guest.send('operator'); guest.wait(lambda s: s.endswith('$ '), timeout=20)
            for mode in modes:
                before = rejected_frees(guest, kernel)
                start = len(guest.data); wall = time.monotonic()
                guest.send(f'sudo /bin/fb-smp-probe --worker 0 {mode}')
                marker = {'siblings': 'FB_SIBLINGS_PASS', 'account': 'FB_ACCOUNT_100_PASS', 'stress': 'FB_STRESS_10S_PASS', 'negative': '[FB CAMPAIGN FAIL] allocation-set drift'}[mode]
                result = guest.wait(lambda s: marker in s or re.search(r'\[FB CAMPAIGN FAIL\][^\n]*\n',s), start, timeout=90)
                assert marker in result, result[-5000:]
                if mode != 'negative': assert '[FB CAMPAIGN FAIL]' not in result
                guest.wait(lambda s: s.endswith('$ '), start, timeout=10)
                after = rejected_frees(guest, kernel)
                assert before == after == 0, ('invalid PMM free', before, after)
                if mode == 'siblings':
                    assert re.findall(r'FB_SIBLING_PASS ([a-z-]+)', result) == ['live','leader-exit','final-thread','process-exit','killed-survivor']
                    assert len(re.findall(r'\[CRASH\] Process: +/bin/fb-smp-probe .*CPU: 1\)', result)) == 2
                    assert result.count('SIGSEGV (11) - Fault: #PF') == 2
                    assert result.count('CR2 = 0x0000006000000000') == 2
                if mode == 'negative':
                    assert 'FB_ACCOUNT_DRIFT cycles=25 ' in result
                    assert 'FB_ACCOUNT_100_PASS' not in result
                if mode == 'account':
                    assert re.findall(r'FB_ACCOUNT_CHECKPOINT cycles=(\d+) bitmap_exact=1', result) == ['25','50','75','100']
                if mode == 'stress':
                    assert set(re.findall(r'FB_STRESS cpu=(\d+).*done=1', result)) == {'0','1','2','3'}
                evidence['modes'].append({'mode': mode, 'result': 'EXPECTED_FAIL' if mode == 'negative' else 'PASS', 'wall_seconds': time.monotonic()-wall, 'rejected_before': before, 'rejected_after': after})
                print(f'PASS {firmware} SMP=4 {mode}', flush=True)
        finally:
            try:
                (output / f'{firmware}.log').write_text(guest.text())
                (output / f'{firmware}.json').write_text(json.dumps(evidence, indent=2))
            finally: guest.close()


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--firmware', nargs='+', choices=['bios','uefi'], default=['bios','uefi'])
    p.add_argument('--modes', nargs='+', choices=['siblings','account','stress','negative'], default=['siblings','account','stress'])
    p.add_argument('--workspace', type=Path)
    args = p.parse_args()
    if 'negative' in args.modes and len(args.modes)!=1: p.error('negative control requires a separate boot')
    output = ROOT / 'build' / f'fb-campaign-{time.time_ns()}'
    output.mkdir(parents=True)
    print(f'Evidence: {output}', flush=True)
    # The campaign builds only in a frozen private copy. Also record enough
    # hashes to distinguish its deliberate observers from production sources.
    production = [ROOT / 'bin' / name for name in ('fortress.elf','fortress.iso','fortress.img','initramfs.tar')]
    before = {str(p.relative_to(ROOT)): digest(p) for p in production if p.is_file()}
    workspace = args.workspace or prepare(output)
    manifest = json.loads((workspace / 'workspace-manifest.json').read_text())
    files = ['src/kernel/thread.c','src/kernel/fb.c','src/kernel/syscall.c',
             'src/mm/pmm.c','src/arch/x86_64/idt.c','src/arch/x86_64/smp.c','src/kernel/fb_campaign_observer.c',
             'src/kernel/fb_campaign.h','tests/fb_smp_probe.c','Makefile']
    manifest['campaign_sources'] = {name: digest(workspace / name) for name in files}
    manifest['production_artifacts_before'] = before
    (output / 'source-manifest.json').write_text(json.dumps(manifest, indent=2))
    try:
        for firmware in args.firmware: boot(workspace, output, firmware, args.modes)
    finally:
        after = {str(p.relative_to(ROOT)): digest(p) for p in production if p.is_file()}
        preserved = before == after
        symbols = subprocess.check_output(['nm', str(ROOT / 'bin/fortress.elf')], text=True) if (ROOT / 'bin/fortress.elf').exists() else ''
        probe_absent = True
        if (ROOT / 'bin/initramfs.tar').exists():
            with tarfile.open(ROOT / 'bin/initramfs.tar') as archive:
                probe_absent = not any(m.name.lstrip('./') == 'bin/fb-smp-probe' for m in archive.getmembers())
        observers_absent = 'fb_campaign_' not in symbols
        (output / 'isolation.json').write_text(json.dumps({'artifacts_preserved': preserved,
            'production_observers_absent': observers_absent, 'production_probe_absent': probe_absent,
            'production_artifacts_after': after}, indent=2))
        assert preserved, 'production build artifacts changed during campaign (including concurrent builds)'
        assert observers_absent and probe_absent, 'test observer/probe leaked into production'



if __name__ == '__main__': main()
