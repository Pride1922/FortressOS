"""Private copied-kernel boot admission; production files are never edited."""
from test_pkg_service_integration import function

def overlay(work,replace):
    host=(work/'tests/pkg_enroll_geometry_host.c').read_text()
    extracted='\n'.join(function(host,s) for s in ('static int identity_path(','static int identities(','static int export_bundle('))
    extracted=extracted.replace('malloc(','kmalloc(').replace('calloc(','kcalloc(').replace('free(','kfree(')
    extracted=extracted.replace('assert(b);','if(!b)return -VFS_ENOMEM;').replace('assert(scratch);','if(!scratch){kfree(b);return -VFS_ENOMEM;}')
    extracted=extracted.replace('uint8_t geometry[1024];','uint8_t *geometry=fs->scratch;').replace('sizeof(geometry)','1024')
    start=extracted.index('FILE *f=fopen(path,"wbx");')
    end=extracted.index('\n    }',start)
    extracted=extracted[:start]+'(void)path;memcpy(jb_bundle,b,at);jb_bundle_n=at;'+extracted[end:]
    (work/'tests/pkg_boot_read_generated.inc').write_text(extracted)
    with (work/'src/fs/ext4.c').open('a') as f:f.write('\n#include "../../tests/pkg_boot_admission.inc"\n')
    replace('src/include/boot_info.h','    uint64_t kernel_file_vaddr, kernel_file_size;',
            '    uint64_t kernel_file_vaddr, kernel_file_size;\n    uint64_t pkg_selector_vaddr,pkg_selector_size,pkg_policy_vaddr,pkg_policy_size;\n    bool pkg_modules_valid;')
    replace('src/kernel/root_boot.c','            else out.valid=false;',
            '            else if(n==21 && !memcmp(token+10,"pkg-test-v1",11))out.mode=ROOT_BOOT_LIVE;\n            else out.valid=false;')
    capture=(work/'tests/pkg_boot_modules.inc').read_text()
    replace('src/kernel/boot_info.c','    root_boot_config_t root=root_boot_parse(',capture+'\n    root_boot_config_t root=root_boot_parse(')
    replace('src/kernel/thread.h','    handoff_registration_t handoff;',
            '    handoff_registration_t handoff;\n    struct {uint64_t generation;uint32_t slot;} pkg_owner;\n    bool pkg_tracked;')
    replace('src/kernel/thread.c','#include "thread.h"','#include "thread.h"\nvoid gk_owner_exit(tcb_t *);\nvoid gk_owner_reap(tcb_t *);')
    for sig in ('void process_exit(uint64_t exit_code) {','void thread_exit(void) {'):
        replace('src/kernel/thread.c',sig,sig+'\n    gk_owner_exit(thread_current());')
    replace('src/kernel/thread.c','            handoff_abandon(&dead->handoff);','            handoff_abandon(&dead->handoff);\n            gk_owner_reap(dead);')
    replace('src/kernel/syscall.c','#include "thread.h"','#include "thread.h"\nint64_t gk_transport(uintptr_t,uintptr_t,uint64_t);\nint64_t gk_test(uint64_t);')
    replace('src/kernel/syscall.c','        case SYS_SIGRETURN:','        case 200:result=gk_transport(frame->rdi,frame->rsi,frame->rdx);break;\n        case 201:result=gk_test(frame->rdi);break;\n        case SYS_SIGRETURN:')
    replace('src/kernel/main.c','    bool installed_ready=!g_installed_boot || installed_root_boot(&boot_info);',
            '    extern void jb_boot(const boot_info_t *,bool,bool);jb_boot(&boot_info,qemu_fw_cfg_has_key("opt/fortress/pkg_boot_nvme"),qemu_fw_cfg_has_key("opt/fortress/pkg_boot_read_error"));\n    bool installed_ready=!g_installed_boot || installed_root_boot(&boot_info);')
    (work/'src/kernel/pkg_boot_private.c').write_text('#include "../../tests/pkg_gate1_kernel.inc"\n')
    for unit in ('pkg_backend_candidate','pkg_writer_candidate','pkg_enrollment_candidate','pkg_mount_candidate','pkg_grant_candidate','pkg_selector_candidate'):
        (work/'src/kernel'/('pkgboot_'+unit+'.c')).write_text('#include "../../tests/'+unit+'.c"\n')
    for unit in ('catalog','verify','format','manifest'):
        (work/'src/kernel'/('pkgboot_'+unit+'.c')).write_text('#include "../../user/pkg/'+unit+'.c"\n')
    (work/'src/kernel/pkgboot_hash.c').write_text('#include "../../src/lib/crypto/hash.c"\n')
    curve=(work/'src/lib/crypto/curve.c').read_text()
    bodies='\n'.join(function(curve,s) for s in ('static void ed_hash_reduce(','static bool canonical(','static bool valid_point(','int crypto_ed25519_verify('))
    (work/'src/kernel/pkgboot_curve.c').write_text('#include "../../src/lib/crypto/internal.h"\n'+bodies+'\n')
    for name,path in (('mono','monocypher.c'),('ed','optional/monocypher-ed25519.c')):
        (work/'src/kernel'/('pkgboot_'+name+'.c')).write_text('#include "../../src/lib/crypto/namespace.h"\n#include "../../src/lib/crypto/upstream/src/'+path+'"\n')
    (work/'src/kernel/pkgboot_gzip.c').write_text('#include "../../user/tools/gzip.c"\n')
    replace('user/tools/common.c','#ifndef TOOL_HOST_TEST','#if !defined(TOOL_HOST_TEST) && !defined(TOOL_KERNEL_CODEC_ONLY)')
    (work/'src/kernel/pkgboot_common.c').write_text('#define TOOL_KERNEL_CODEC_ONLY\n#include "../../user/tools/common.c"\nlong tool_syscall(long n,uintptr_t a,uintptr_t b,uintptr_t c){(void)n;(void)a;(void)b;(void)c;return SYSCALL_EOPNOTSUPP;}\n')
    with (work/'Makefile').open('a') as f:f.write('\nCFLAGS += -I. -Itests -Isrc/lib/crypto/upstream/src -fstack-usage\nLDFLAGS += --gc-sections\nbuild/kernel/pkgboot_mono.o build/kernel/pkgboot_ed.o build/kernel/pkgboot_curve.o build/kernel/pkgboot_hash.o: CFLAGS += -Os -ffunction-sections -fdata-sections\n')
    return {'extracted_native_readback':extracted,'scope':'private copied workspace only'}
