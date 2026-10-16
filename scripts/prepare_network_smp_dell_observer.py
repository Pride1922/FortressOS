"""Build an isolated live Dell observer image; never writes a physical device.

The production Makefile and syscall ABI are unchanged. This reuses the Phase 3
CS=3 instrumentation, adds ordinary DHCP observation, and a bounded read-only
private snapshot call so the existing observer prints through its own stdout.
"""
import argparse,hashlib,json,shutil,subprocess,tarfile,tempfile
from pathlib import Path
import test_network_smp_phase3 as observer

ROOT=Path(__file__).resolve().parents[1]
def digest(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

def instrument(workspace):
    observer.COMMANDS=observer.COMMANDS+['/sbin/dhcpc']
    observer.instrument(workspace)
    path=workspace/'src/kernel/syscall.c';text=path.read_text()
    text='#include "types.h"\nstatic uint64_t phase4_boot_dhcp;\nstatic int64_t phase4_snapshot(uint64_t,uintptr_t,uint64_t);\n'+text
    anchor='    for (unsigned i=0;i<PHASE3_COMMANDS;i++) if (!strcmp(self->name,phase3_names[i])) {'
    assert text.count(anchor)==1
    text=text.replace(anchor,'''    /* Trusted initial DHCP probe is deliberately BSP-pinned; do not classify
     * it as an ordinary VFS child or change its placement. */
    if (!strcmp(self->name,"/sbin/dhcpc") && self->cpu_affinity>=0) {
        if (self->cpu_affinity!=0 || self->cpus_allowed!=CPU_MASK_ONE(0) || cpu_current()->id) __builtin_trap();
        __atomic_fetch_or(&phase4_boot_dhcp,1ULL<<cpu_current()->id,__ATOMIC_RELEASE);
        return;
    }
'''+anchor)
    anchor='    switch (syscall_nr) {';assert text.count(anchor)==1
    text=text.replace(anchor,anchor+'''
        case 0x7ff6:
            result=(frame->cs&3)==3 ? phase4_snapshot(frame->rdi,frame->rsi,frame->rdx) : SYSCALL_EPERM;
            break;
''')
    text+='''
#include "../../tests/net_cpu_observation.h"
static int64_t phase4_snapshot(uint64_t row,uintptr_t address,uint64_t bytes) {
    if (row>=PHASE3_COMMANDS) return SYSCALL_ENOENT;
    if (bytes!=sizeof(net_cpu_observation_t)) return SYSCALL_EINVAL;
    if (!vmm_validate_user_range(vmm_get_active_pml4_virt(),address,bytes,true)) return SYSCALL_EFAULT;
    net_cpu_observation_t snapshot={0};
    snapshot.version=NET_CPU_OBSERVATION_VERSION;
    for (unsigned i=0;i<sizeof(snapshot.name)-1 && phase3_names[row][i];i++) snapshot.name[i]=phase3_names[row][i];
    snapshot.owner_cpus=__atomic_load_n(&phase3_owner_cpus,__ATOMIC_ACQUIRE);
    snapshot.owner_steps=__atomic_load_n(&phase3_owner_steps,__ATOMIC_ACQUIRE);
    snapshot.boot_dhcp_cpus=__atomic_load_n(&phase4_boot_dhcp,__ATOMIC_ACQUIRE);
    for (unsigned nr=0;nr<NET_CPU_OBSERVATION_SYSCALLS;nr++) snapshot.masks[nr]=__atomic_load_n(&phase3_masks[row][nr],__ATOMIC_ACQUIRE);
    memcpy((void *)address,&snapshot,sizeof(snapshot));
    return 0;
}
'''
    path.write_text(text)
    # Live diagnostic boot has no installed-root admission or data mount request.
    (workspace/'limine.conf').write_text('''timeout: 0
/FortressOS DELL CPU OBSERVER TEST - live
    protocol: limine
    kernel_path: boot():/boot/fortress.elf
    module_path: boot():/boot/initramfs.tar
    kernel_cmdline: boot_mode=live net=dhcp verbose
''')
    path=workspace/'scripts/create_boot_img.py';text=path.read_text()
    import re
    text,n=re.subn(r'kernel_cmdline: usb_data=PARTUUID=\{part2_guid_str\} usb_data_mode=(?:rw|ro)(?: verbose net=dhcp)?',
                  'kernel_cmdline: boot_mode=live net=dhcp verbose',text)
    assert n==3,n
    text=text.replace('/FortressOS (','/FortressOS DELL CPU OBSERVER TEST (')
    path.write_text(text)

def main():
    p=argparse.ArgumentParser();p.add_argument('--output-parent',type=Path,default=ROOT/'build/network-smp-phase4');args=p.parse_args()
    args.output_parent.mkdir(parents=True,exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='dell-observer-',dir=args.output_parent));print(out,flush=True)
    result={'result':'FAIL','scope':'live observer TEST image, no installed-root/data mount request, no physical writes'}
    normal={str(p):digest(p) for p in (ROOT/'bin').glob('*') if p.suffix in ('.img','.iso')}
    try:
        workspace=Path(subprocess.check_output(['python3','scripts/create_ext4_guest_workspace.py'],cwd=ROOT,text=True).strip())
        result['workspace']=str(workspace);result['sources']=json.loads((workspace/'workspace-manifest.json').read_text())
        instrument(workspace)
        result['snapshot_overrides']={n:digest(workspace/n) for n,h in result['sources']['sources'].items() if digest(workspace/n)!=h}
        def run(argv,timeout=600):
            result.setdefault('commands',[]).append(list(map(str,argv)))
            with (out/'build.log').open('ab') as log:subprocess.run(list(map(str,argv)),cwd=workspace,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=timeout)
        run(['make','-j4','bin/fortress.elf','bin/initramfs.tar'])
        run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-ffreestanding','-mno-red-zone','-mgeneral-regs-only','-fno-pie','-fno-stack-protector','-Isrc/include','-c','tests/net_ordinary_observer.c','-o',out/'observer.o'])
        run(['nasm','-f','elf64','-DTOOL_ENTRY=net_cpu_probe_main','user/tools/start.asm','-o',out/'observer-start.o'])
        run(['ld','-m','elf_x86_64','-nostdlib','-static','-z','noexecstack','-T','user/shell.ld',out/'observer-start.o',out/'observer.o','-o',out/'net-cpu-probe.elf'])
        # Make regenerates initramfs during media targets. Inject the command
        # into the snapshot-only staging recipe, rather than appending a tar
        # member that a later normal rebuild would discard.
        path=workspace/'Makefile';text=path.read_text()
        anchor='\t@python3 scripts/create_initramfs.py $(BUILD_DIR)/initramfs $(INITRAMFS_TAR)'
        assert text.count(anchor)==1
        text=text.replace(anchor,'\t@cp -f '+str(out/'net-cpu-probe.elf')+' $(BUILD_DIR)/initramfs/bin/net-cpu-probe\n'+anchor)
        path.write_text(text)
        result['snapshot_overrides']['Makefile']=digest(path)
        run(['make','-j4','bin/fortress.iso','bin/fortress.img'])
        with tarfile.open(workspace/'bin/initramfs.tar') as archive:
            assert archive.extractfile('bin/net-cpu-probe').read()==(out/'net-cpu-probe.elf').read_bytes()
        symbols=subprocess.check_output(['nm',str(workspace/'bin/fortress.elf')],text=True)
        assert all(n in symbols for n in ('phase3_entry','phase3_masks','phase4_snapshot'))
        assert not any(n in symbols for n in ('fixture_accept','net_tcp_fixture','net_socket_fixture'))
        result['artifacts']={}
        for name in ('fortress.elf','initramfs.tar','fortress.iso','fortress.img'):
            dest=out/('fortress-dell-cpu-observer'+Path(name).suffix if name.startswith('fortress.') else name)
            shutil.copyfile(workspace/'bin'/name,dest)
            result['artifacts'][dest.name]={'bytes':dest.stat().st_size,'sha256':digest(dest)}
        assert all(digest(Path(p))==h for p,h in normal.items())
        result['shared_images_unchanged']=normal
        assert all(digest(ROOT/n)==h for n,h in result['sources']['sources'].items())
        result['result']='PASS'
    finally:
        (out/'manifest.json').write_text(json.dumps(result,indent=2)+'\n')
    print(out,flush=True)
if __name__=='__main__':main()
