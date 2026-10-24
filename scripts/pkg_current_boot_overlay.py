"""Private current-endpoint layer over the accepted measured boot overlay."""
from pkg_boot_admission_overlay import overlay as base_overlay

def overlay(work,replace):
    result=base_overlay(work,replace)
    replace('src/kernel/boot_info.c','m->size==1136','m->size>=1248 && m->size<=33920')
    path='tests/pkg_boot_admission.inc'
    replace(path,'#include "pkg_selector_candidate.h"','#include "pkg_selector_candidate.h"\n#include "pkg_current_candidate.h"')
    replace(path,'static ps_workspace jb_workspace;','static pc_workspace jc_workspace;\nstatic pc_result jc_result;\nstatic uint8_t jc_baseline[PS_BUNDLE_MAX],jc_artifact[8192],jc_executable[8192];\nstatic size_t jc_baseline_n,jc_artifact_n,jc_executable_n;\nstatic uint64_t jc_grant_floor,jc_state_floor;')
    replace(path,'#include "pkg_boot_read_generated.inc"','#include "pkg_boot_read_generated.inc"\n#include "pkg_current_fs.inc"')
    replace(path,'"origin.v1","account-generation.v1","state.v1","trust.v1","descriptor.v2"};',
            '"origin.v1","account-generation.v1","state.v1","trust.v1","descriptor.v2","artifact.fpk"};')
    replace(path,'unsigned count=directory?0:9,seen=0;','unsigned count=directory==0?10:directory==2?1:0,seen=0;')
    replace(path,'strcmp(entry.name,names[child])','strcmp(entry.name,directory==2?"demo":names[child])')
    replace(path,'    if(!r && ps_validate(jb_selector,jb_selector_n,jb_bundle,jb_bundle_n,&jb_policy,&jb_workspace,&jb_result))r=-VFS_EACCES;',
'''    if(!r)r=current_read_file(fs,"/pkg/artifact.fpk",0600,jc_artifact,&jc_artifact_n);
    if(!r)r=current_read_file(fs,"/usr/bin/demo",0755,jc_executable,&jc_executable_n);
    pc_input input={.selector={jb_selector,jb_selector_n},.baseline={jc_baseline,jc_baseline_n},
        .current={jb_bundle,jb_bundle_n},.artifact={jc_artifact,jc_artifact_n},.executable={jc_executable,jc_executable_n},
        .policy=&jb_policy,.grant_floor=jc_grant_floor,.state_floor=jc_state_floor};
    if(!r && (pc_validate(&input,&jc_workspace,&jc_result) || !jc_result.installed))r=-VFS_EACCES;
    if(!r)jb_result=jc_result.enrollment;''')
    replace(path,'    memcpy(&jb_policy,(const void *)b->pkg_policy_vaddr,sizeof jb_policy);',
'''    memcpy(&jb_policy,(const void *)b->pkg_policy_vaddr,sizeof jb_policy);
    if(b->pkg_policy_size<1248 || b->pkg_policy_size>33920)goto refused;
    memcpy(&jc_grant_floor,(const uint8_t *)b->pkg_policy_vaddr+1136,8);
    memcpy(&jc_state_floor,(const uint8_t *)b->pkg_policy_vaddr+1144,8);
    jc_baseline_n=b->pkg_policy_size-1152;
    memcpy(jc_baseline,(const uint8_t *)b->pkg_policy_vaddr+1152,jc_baseline_n);''')
    replace(path,'PKGBOOT ADMITTED IDLE=1 detached','PKGBOOT ADMITTED IDLE=2 detached')
    replace(path,'.phase=PKG_TX_IDLE,.sequence=1,.account_sequence=1,.credential_generation=1',
            '.phase=PKG_TX_IDLE,.sequence=2,.account_sequence=1,.credential_generation=1')
    for operation in ('PB_STATUS,0','PB_RECOVER,session'):
        before=f'need(!call({operation},&r)&&r.phase==PKG_TX_IDLE&&r.sequence==1);'
        replace('tests/pkg_boot_ring3.c',before,before.replace('r.sequence==1','r.sequence==2'))
    (work/'src/kernel/pkgboot_current.c').write_text('#include "../../tests/pkg_current_candidate.c"\n')
    result['current_profile']='immutable signed baseline plus completed IDLE demo; external grant/state floors'
    return result
