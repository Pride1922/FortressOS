"""Private contract 3: signed, journaled floors before content and root publish."""
from pkg_running_root_overlay import overlay as running

def overlay(work,replace):
    result=running(work,replace)
    def every(name,before,after,count=None):
        text=(work/name).read_text();found=text.count(before);assert found and (count is None or found==count),(name,before,found);replace(name,text,text.replace(before,after))
    for path in ('src/kernel/main.c','src/kernel/boot_info.c','tests/pkg_boot_admission.inc'):
        every(path,'boot_mode=pkg-test-v1','boot_mode=pkg-trust-v2')
    replace('src/kernel/root_boot.c','n==21 && !memcmp(token+10,"pkg-test-v1",11)','n==22 && !memcmp(token+10,"pkg-trust-v2",12)')
    p='tests/pkg_boot_admission.inc'
    replace(p,'#include "pkg_pending_fs.inc"','#include "pkg_pending_fs.inc"\n#include "pkg_trust_fs.inc"')
    replace(p,'    pn_baseline_n=b->pkg_policy_size-1152;','    if(b->pkg_policy_size<1152+96+3072)goto refused;\n    pn_baseline_n=e4_u32((const uint8_t *)b->pkg_policy_vaddr+1152+16);\n    if(pn_baseline_n<96 || pn_baseline_n>PS_BUNDLE_MAX || b->pkg_policy_size!=1152+pn_baseline_n+3072)goto refused;\n    memcpy(ft_module,(const uint8_t *)b->pkg_policy_vaddr+1152+pn_baseline_n,3072);ft_active=true;')
    replace(p,'    if(!r)r=pn_load(fs);','    if(!r)r=pn_load(fs);\n    if(!r)r=ft_load(fs);')
    p='tests/pkg_pending_fs.inc'
    replace(p,'static const char *names[][10]', 'static const char *names[][11]')
    replace(p,'"descriptor.v2","artifact.fpk"}', '"descriptor.v2","artifact.fpk","trust-floor.v1"}')
    replace(p,'count=d==0?10:', 'count=d==0?11:')
    replace(p,'seen!=4095','seen!=8191')
    p='tests/pkg_running_root.inc'
    replace(p,'    stage="package recovery";',
'''    stage="pre-content trust floor";
    {uint64_t flags=spin_lock_irqsave(&e4_lock);jb_authorized=true;r=ft_publish(jb_engine,1);jb_authorized=false;spin_unlock_irqrestore(&e4_lock,flags);if(r)goto fail;}
    stage="package recovery";''')
    replace(p,'    stage="measured executables";',
'''    stage="completion trust floor";
    {uint64_t flags=spin_lock_irqsave(&e4_lock);jb_authorized=true;r=ft_publish(jb_engine,2);jb_authorized=false;spin_unlock_irqrestore(&e4_lock,flags);if(r)goto fail;}
    stage="measured executables";''')
    replace(p,'    for(unsigned i=0;i<5;i++){int r=pn_fence_object(e,jh_base+i);if(r)return r;}','    for(unsigned i=0;i<5;i++){int r=pn_fence_object(e,jh_base+i);if(r)return r;}\n    if(ft_active){int r=pn_fence_object(e,&ft_object);if(r)return r;}')
    (work/'src/kernel/pkgboot_trust_floor.c').write_text('#include "../../tests/pkg_trust_floor.c"\n')
    result['trust']='contract 3, independent signed minimum/proposals, pre-content and completion durable floors'
    return result
