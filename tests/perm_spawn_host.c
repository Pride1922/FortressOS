#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "thread.h"
#include "spawn_security.h"
#include "elf.h"
#define PAGE_SIZE 4096
#include "../user/entry_security.h"
static unsigned char stack[4096];
void *vmm_phys_to_virt(uintptr_t p) {(void)p;return stack;}
/* Actual loader stack function is extracted verbatim by the host driver. */
#include "stack_function.inc"
int main(void) {
    unsigned cases=0;
    for (unsigned mode=0;mode<4096;mode++) for (unsigned nosuid=0;nosuid<2;nosuid++)
    for (unsigned root=0;root<2;root++) for (unsigned dropped=0;dropped<2;dropped++) {
        creds_t actor={.uid=1000,.euid=root ? 0:1000,.suid=1000,.gid=1000,.egid=1000,.sgid=1000,
            .groups={10,44},.ngroups=2,.umask=0027,.cap_effective=dropped ? 0:CAP_KILL};
        vfs_metadata_t image={.mode=VFS_S_IFREG|mode,.uid=0,.gid=42,.mnt_flags=nosuid ? VFS_MNT_NOSUID:0};
        creds_t out;bool secure=spawn_credentials(&actor,&image,&out);
        bool u=!nosuid && (mode & 04000),g=!nosuid && (mode & 02000) && (mode & 0010);
        assert(secure==(u || g));assert(out.uid==1000 && out.gid==1000 && out.umask==0027);
        assert(out.euid==(u ? 0:actor.euid) && out.suid==(u ? 0:actor.suid));
        assert(out.egid==(g ? 42:1000) && out.sgid==(g ? 42:1000));
        assert(out.cap_effective==(u && !root ? CAP_ALL:actor.cap_effective));
        assert(out.ngroups==2 && out.groups[0]==10 && out.groups[1]==44);cases++;
    }
    creds_t high={.uid=1000,.euid=1000,.suid=1000,.gid=1000,.egid=1000,.sgid=1000,.cap_effective=CAP_KILL},next;
    vfs_metadata_t high_image={.mode=VFS_S_IFREG|06755,.uid=UINT32_MAX,.gid=UINT32_MAX};
    assert(spawn_credentials(&high,&high_image,&next));
    assert(next.euid==UINT32_MAX && next.suid==UINT32_MAX && next.egid==UINT32_MAX && !next.cap_effective);
    spawn_kaction_t actions[]={{.type=SPAWN_FD_ACTION_DUP2,.src_fd=31,.dst_fd=7},
        {.type=SPAWN_FD_ACTION_CLOSE,.dst_fd=7},{.type=SPAWN_FD_ACTION_OPEN,.dst_fd=9}};
    assert(spawn_mapped_fds(0,NULL)==7);
    assert(spawn_mapped_fds(3,actions)==(7|(1u<<9)));
    uintptr_t rsp,argv,envp;
    const char *args[]={"sudo","id"},*env[]={"PATH=/evil","HOME=/tmp"};
    assert(!process_setup_user_stack(1,2,args,2,env,&rsp,&argv,&envp));
    assert(!(rsp & 15));
    uint64_t *table=(uint64_t *)(stack+rsp-USER_STACK_PAGE_VIRT);
    assert(table[0]==2 && table[3]==0 && table[6]==0 && table[7]==23 && table[8]==0 && table[9]==0 && table[10]==0);
    assert(!process_setup_user_stack(1,0,NULL,0,NULL,&rsp,&argv,&envp));
    table=(uint64_t *)(stack+rsp-USER_STACK_PAGE_VIRT);
    assert(table[3]==23 && table[4]==0 && table[5]==0 && table[6]==0);
    uintptr_t entry[]={0,23,1,0,0};assert(user_entry_secure((const char *const *)entry));
    entry[2]=0;assert(!user_entry_secure((const char *const *)entry));
    const char *maximum[32];char text[32];memset(text,'x',31);text[31]=0;
    for (unsigned i=0;i<32;i++) maximum[i]=text;
    assert(!process_setup_user_stack(1,32,maximum,32,maximum,&rsp,&argv,&envp));
    assert(rsp>=USER_STACK_PAGE_VIRT+MINIMUM_USER_STACK_FLOOR && !(rsp&15));
    assert(process_setup_user_stack(1,33,maximum,0,NULL,&rsp,&argv,&envp)<0);
    printf("PASS %u set-ID credential vectors; fd destinations and actual stack/AT_SECURE\n",cases);
}
