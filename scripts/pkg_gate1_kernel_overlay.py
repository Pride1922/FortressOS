"""Private freestanding extraction; exact source bodies and edits are recorded.

No host translation unit is compiled into the kernel. Function dependencies are
closed over the accepted copied-source adapter, with fixture construction,
signing, diagnostics and test mains excluded. No production source is edited.
"""
import hashlib,re,textwrap
from test_pkg_service_integration import function
from pkg_account_service_overlay import apply
from pkg_mount_overlay import apply_mount
from pkg_cli_transaction_overlay import apply_cli

def overlay(work,replace):
    original_accounts=(work/'src/kernel/accounts.c').read_bytes()
    text=(work/'scripts/test_pkg_mount_admission.py').read_text()
    a=text.index("    replace('src/fs/ext4_transaction.inc'");b=text.index('    env=dict(os.environ)',a)
    scope=dict(work=work,replace=replace,function=function,apply=apply,apply_mount=apply_mount)
    exec(compile(textwrap.dedent(text[a:b]),'private-accepted-overlay','exec'),scope)
    apply_cli(work,replace,scope['generated'])
    # Account authentication for the live boot root stays unchanged. The private
    # detached mount is never published to the ordinary account service.
    (work/'src/kernel/accounts.c').write_bytes(original_accounts)
    files=['pkg_account_durable.inc','pkg_account_service_durable.inc','pkg_mount_admission.inc',
           'pkg_retirement_integrated.inc','pkg_retirement_idle.inc','pkg_private_completion.inc']
    sources={name:(work/'tests'/name).read_text() for name in files}
    definitions={}
    for name,source in sources.items():
        for m in re.finditer(r'^static\s+[^;{}\n]+?\b((?:ad|sv|ng|xr|xi)_[A-Za-z0-9_]+)\([^;{}]*?\)\s*\{',source,re.M):
            body=function(source,m[0][:-1].rstrip())
            definitions[m[1]]=(name,body)
    overrides={
      'sv_path':'''static const char *sv_path(unsigned role){
        if(role>=ACCOUNT_ROLE_COUNT)return "";
        if(!sv_paths[role][0]){size_t n=strlen(e4_ac_paths[role]);if(n+5>sizeof sv_paths[role])return "";
          memcpy(sv_paths[role],"/mnt",4);memcpy(sv_paths[role]+4,e4_ac_paths[role],n+1);}
        return sv_paths[role];
      }''',
      'ng_gate':'static int ng_gate(ext4_engine_t *e,bool planned){return xi_gate(e,planned);}',
      'ng_extra':'''static int ng_extra(void){
        for(unsigned i=436;i<440;i++)if(ad_state[i])return -VFS_EIO;
        for(unsigned i=624;i<992;i++)if(ad_state[i])return -VFS_EIO;
        return 0;
      }''',
      'xi_archive_commit':'static int xi_archive_commit(ext4_engine_t *e){(void)e;return -VFS_EROFS;}',
    }
    edits=[]
    for name,body in overrides.items():
        if name in definitions:edits.append(dict(function=name,before=definitions[name][1],after=body))
        definitions[name]=('private-port',body)
    name,body=definitions['xi_archive_gate']
    stripped=re.sub(r'fprintf\(stderr,"(?:[^"\\]|\\.)*"[^;]*;', '',body)
    edits.append(dict(function='xi_archive_gate',before=body,after=stripped));definitions['xi_archive_gate']=(name,stripped)
    name,body=definitions['xi_manual_step']
    stripped=body.replace('if(xi_terminal_pause&&xi_stage==10)ng_pause=true;','')
    edits.append(dict(function='xi_manual_step',before=body,after=stripped));definitions['xi_manual_step']=(name,stripped)
    name,body=definitions['xi_finish']
    stripped=body.replace(' for(unsigned steps=0;steps<16;steps++){',' for(unsigned steps=0;steps<16;steps++){\n  if(gk_boundary())return -VFS_EINTR;')
    stripped=stripped.replace('int r=ad_validate(e);','int r=e->tainted?-VFS_EIO:ad_validate(e);')
    assert stripped!=body
    edits.append(dict(function='xi_finish',before=body,after=stripped));definitions['xi_finish']=(name,stripped)
    roots=['ad_validate','xi_gate','xi_finish','xi_commit','xi_activate','xi_retire','xr_preflight','xr_reopened','xr_policy_load','ng_policy_load']
    selected={}
    def visit(name):
        if name in selected:return
        source,body=definitions[name];selected[name]=(source,body)
        for called in re.findall(r'\b([a-zA-Z_]\w*)\s*\(',body):
            if called in definitions:visit(called)
    for name in roots:visit(name)
    # xi_digest precedes the extracted core; add it as a dependency normally.
    assert 'xi_digest' in selected
    config=(work/'tests/pkg_gate1_engine_config.inc').read_text()
    prototypes='\n'.join(body[:body.index('{')].strip()+';' for _,body in selected.values())
    core=config+'\n'+prototypes+'\n'+'\n'.join(body for _,body in selected.values())
    for forbidden in ('fprintf(', 'printf(', 'snprintf(', 'malloc(', 'free(', 'ad_disk', 'load(', 'save(', 'tool_syscall'):
        # kmalloc/kfree are freestanding allocator calls, not host malloc/free.
        assert not re.search(r'(?<![\w])'+re.escape(forbidden),core),forbidden
    (work/'tests/pkg_gate1_engine_generated.inc').write_text(core)
    # The core's preflight hooks must be declared before the included EXT4
    # primitives. Kernel-only context and callbacks are appended after them.
    pre='''
#include "thread.h"
#include "serial.h"
#include "smp.h"
#include "nvme.h"
#include "crypto.h"
#include "account_redo.h"
#include "../../user/tools/userdb.h"
#include "../../user/tools/digest.h"
#include "pkg_backend_candidate.h"
#include "pkg_retirement_auth.h"
#define assert(x) do {if(!(x)){serial_raw_puts("GATE1 ASSERT ");serial_raw_print_dec(__LINE__);__builtin_trap();}}while(0)
static bool xr_active,xr_typed,ng_pause;
static struct ext4_mount *sv_fs;
static int xi_gate(struct ext4_engine *,bool);
static int xi_activate(unsigned,unsigned);
static int xi_retire(unsigned);
static int xr_preflight(struct ext4_engine *);
static int xr_reopened(struct ext4_engine *);
static int ad_preview(struct ext4_engine *);
static int ad_activate(struct ext4_engine *,unsigned,unsigned);
static int ad_retire(struct ext4_engine *,unsigned);
static int e4_semantic_commit(struct ext4_engine *);
static int e4_namespace_end(struct ext4_engine *,int);
static bool sv_pin_deny(struct vfs_node *);
static bool gk_boundary(void);
'''
    replace('src/fs/ext4.c','#include "ext4.h"','#include "ext4.h"\nstruct ext4_engine;\n'+pre)
    with (work/'src/fs/ext4.c').open('a') as f:f.write('\n#include "../../tests/pkg_gate1_engine_generated.inc"\n#include "../../tests/pkg_gate1_storage.inc"\n')
    replace('src/kernel/thread.h','    handoff_registration_t handoff;',
            '    handoff_registration_t handoff;\n    struct {uint64_t generation;uint32_t slot;} pkg_owner;\n    bool pkg_tracked;')
    replace('src/kernel/thread.c','#include "thread.h"','#include "thread.h"\nvoid gk_owner_exit(tcb_t *);\nvoid gk_owner_reap(tcb_t *);')
    for sig in ('void process_exit(uint64_t exit_code) {','void thread_exit(void) {'):
        replace('src/kernel/thread.c',sig,sig+'\n    gk_owner_exit(thread_current());')
    replace('src/kernel/thread.c','            handoff_abandon(&dead->handoff);','            handoff_abandon(&dead->handoff);\n            gk_owner_reap(dead);')
    replace('src/kernel/syscall.c','#include "thread.h"','#include "thread.h"\nint64_t gk_transport(uintptr_t,uintptr_t,uint64_t);\nint64_t gk_test(uint64_t);')
    replace('src/kernel/syscall.c','        case SYS_SIGRETURN:',
            '        case 200: result=gk_transport(frame->rdi,frame->rsi,frame->rdx);break;\n        case 201: result=gk_test(frame->rdi);break;\n        case SYS_SIGRETURN:')
    replace('src/kernel/main.c','    bool installed_ready=!g_installed_boot || installed_root_boot(&boot_info);',
            '    extern void gk_fixture_boot(bool,bool);gk_fixture_boot(qemu_fw_cfg_has_key("opt/fortress/pkg_gate1_nvme"),qemu_fw_cfg_has_key("opt/fortress/pkg_gate1_read_error"));\n    bool installed_ready=!g_installed_boot || installed_root_boot(&boot_info);')
    (work/'src/kernel/pkg_gate1_private.c').write_text('#include "../../tests/pkg_gate1_kernel.inc"\n')
    units=['pkg_backend_candidate','pkg_writer_candidate','pkg_enrollment_candidate','pkg_mount_candidate','pkg_grant_candidate','pkg_retirement_candidate','pkg_retirement_auth']
    for unit in units:(work/'src/kernel'/('gate1_'+unit+'.c')).write_text('#include "../../tests/'+unit+'.c"\n')
    for unit in ('catalog','verify','format','manifest'):
        (work/'src/kernel'/('gate1_'+unit+'.c')).write_text('#include "../../user/pkg/'+unit+'.c"\n')
    (work/'src/kernel/gate1_crypto_hash.c').write_text('#include "../../src/lib/crypto/hash.c"\n')
    curve_source=(work/'src/lib/crypto/curve.c').read_text()
    curve='\n'.join(function(curve_source,sig) for sig in ('static void ed_hash_reduce(','static bool canonical(','static bool valid_point(','int crypto_ed25519_verify('))
    (work/'src/kernel/gate1_crypto_verify.c').write_text('#include "../../src/lib/crypto/internal.h"\n'+curve+'\n')
    for name,path in [('mono','monocypher.c'),('ed','optional/monocypher-ed25519.c')]:
        (work/'src/kernel'/('gate1_crypto_'+name+'.c')).write_text('#include "../../src/lib/crypto/namespace.h"\n#include "../../src/lib/crypto/upstream/src/'+path+'"\n')
    # Existing kernel objects already provide digest/userdb/crypto; gzip and
    # common are needed by the actual bounded verifier, in their own TU.
    (work/'src/kernel/gate1_gzip.c').write_text('#include "../../user/tools/gzip.c"\n')
    replace('user/tools/common.c','#ifndef TOOL_HOST_TEST','#if !defined(TOOL_HOST_TEST) && !defined(TOOL_KERNEL_CODEC_ONLY)')
    (work/'src/kernel/gate1_common.c').write_text('#define TOOL_KERNEL_CODEC_ONLY\n#include "../../user/tools/common.c"\nlong tool_syscall(long n,uintptr_t a,uintptr_t b,uintptr_t c){(void)n;(void)a;(void)b;(void)c;return SYSCALL_EOPNOTSUPP;}\n')
    with (work/'Makefile').open('a') as f:f.write('\nCFLAGS += -I. -Itests -Isrc/lib/crypto/upstream/src -fstack-usage\nLDFLAGS += --gc-sections\nbuild/kernel/gate1_crypto_mono.o build/kernel/gate1_crypto_ed.o build/kernel/gate1_crypto_hash.o build/kernel/gate1_crypto_verify.o: CFLAGS += -Os -ffunction-sections -fdata-sections\n')
    return dict(functions={name:dict(source=src,sha256=hashlib.sha256(body.encode()).hexdigest()) for name,(src,body) in selected.items()},edits=edits,generated_sha256=hashlib.sha256(core.encode()).hexdigest())
