/* Actual pure value engine against externally generated Python vectors. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "permission_values.h"

static int expected(FILE *input) {
    int byte = fgetc(input);
    assert(byte != EOF);
    return byte > 127 ? byte - 256 : byte;
}
static void finish(FILE *input) { assert(fgetc(input) == EOF); assert(!fclose(input)); }
static creds_t actor(unsigned identity, uint64_t caps) {
    creds_t value = {.uid=1001, .euid=1001, .suid=1001,
        .gid=1002, .egid=1002, .sgid=1002, .cap_effective=caps};
    if (identity == 0) value.euid = 42;
    if (identity == 1) value.egid = 43;
    if (identity == 2) { value.groups[0] = 43; value.ngroups = 1; }
    return value;
}
int main(int argc, char **argv) {
    assert(argc == 5);
    FILE *input = fopen(argv[1], "rb"); assert(input);
    uint64_t caps[] = {0, CAP_DAC_OVERRIDE, CAP_DAC_READ_SEARCH};
    unsigned long count = 0;
    for (unsigned mode = 0; mode < 4096; ++mode)
    for (unsigned mask = 1; mask < 8; ++mask)
    for (unsigned identity = 0; identity < 4; ++identity)
    for (unsigned cap = 0; cap < 3; ++cap)
    for (unsigned directory = 0; directory < 2; ++directory)
    for (unsigned ro = 0; ro < 2; ++ro) {
        vfs_metadata_t value = {.mode=(directory ? VFS_S_IFDIR : VFS_S_IFREG)|mode,
            .uid=42, .gid=43, .mnt_flags=ro ? VFS_MNT_RDONLY : 0};
        creds_t cr = actor(identity, caps[cap]);
        int want = expected(input), got = permission_access_value(&value, mask, &cr);
        if (got != want) {
            fprintf(stderr, "mode=%o mask=%u identity=%u caps=%u dir=%u ro=%u: %d != %d\n",
                mode,mask,identity,cap,directory,ro,got,want); abort();
        }
        ++count;
    }
    finish(input); printf("PASS DAC value matrix: %lu cases\n", count);
    input = fopen(argv[2], "rb"); assert(input);
    unsigned masks[] = {0,1,7,0022,0027,0077,0700,0777};
    count = 0;
    for (unsigned mode = 0; mode < 4096; ++mode)
    for (unsigned m = 0; m < sizeof(masks)/sizeof(masks[0]); ++m)
    for (unsigned setgid = 0; setgid < 2; ++setgid)
    for (unsigned member = 0; member < 3; ++member)
    for (unsigned cap = 0; cap < 2; ++cap)
    for (unsigned directory = 0; directory < 2; ++directory) {
        vfs_metadata_t dir = {.mode=VFS_S_IFDIR|0777|(setgid ? 02000 : 0), .gid=43};
        creds_t cr = actor(member == 0 ? 1 : member == 1 ? 2 : 3,
            cap ? CAP_FSETID : 0); cr.umask = masks[m];
        vfs_create_attrs_t out;
        assert(!permission_create_value(&dir,directory ? VFS_DIRECTORY : VFS_FILE,mode,&cr,&out));
        unsigned want = (unsigned)fgetc(input); want |= (unsigned)fgetc(input) << 8;
        assert(out.mode == want && out.uid == cr.euid && out.gid == (setgid ? 43 : cr.egid));
        ++count;
    }
    finish(input); printf("PASS create value matrix: %lu cases\n", count);
    input = fopen(argv[3], "rb"); assert(input);
    unsigned boundary_modes[] = {0,0777,01777,02777,06777,07777,0xffffffffu};
    count = 0;
    for (unsigned mask = 0; mask < 512; ++mask)
    for (unsigned m = 0; m < sizeof(boundary_modes)/sizeof(boundary_modes[0]); ++m)
    for (unsigned directory = 0; directory < 2; ++directory) {
        vfs_metadata_t dir = {.mode=VFS_S_IFDIR|02777,.gid=43};
        creds_t cr=actor(3,0); cr.umask=mask;
        vfs_create_attrs_t out;
        assert(!permission_create_value(&dir,directory ? VFS_DIRECTORY : VFS_FILE,boundary_modes[m],&cr,&out));
        unsigned want=(unsigned)fgetc(input); want |= (unsigned)fgetc(input) << 8;
        assert(out.mode==want); ++count;
    }
    finish(input); printf("PASS every umask and requested-mode bounds: %lu cases\n",count);

    input=fopen(argv[4],"rb"); assert(input); count=0;
    uint64_t mode_caps[]={0,CAP_FOWNER,CAP_FSETID,CAP_FOWNER|CAP_FSETID};
    for (unsigned mode=0;mode<4096;++mode)
    for (unsigned member=0;member<2;++member)
    for (unsigned cap=0;cap<4;++cap)
    for (unsigned owner=0;owner<2;++owner)
    for (unsigned ro=0;ro<2;++ro) {
        vfs_metadata_t old={.mode=VFS_S_IFREG|06754,.uid=owner ? 1001 : 42,.gid=43,
            .mnt_flags=ro ? VFS_MNT_RDONLY : 0,.size=123};
        creds_t cr=actor(member ? 2 : 3,mode_caps[cap]);
        vfs_metadata_t next; memset(&next,0xa5,sizeof(next));
        vfs_metadata_t sentinel; memcpy(&sentinel,&next,sizeof(next));
        int want=expected(input);
        unsigned want_mode=(unsigned)fgetc(input); want_mode|=(unsigned)fgetc(input)<<8;
        assert(permission_chmod_value(&old,mode,&cr,&next)==want);
        if (want) assert(!memcmp(&sentinel,&next,sizeof(next)));
        else assert(next.mode==want_mode && next.uid==old.uid && next.gid==old.gid && next.size==old.size);
        ++count;
    }
    finish(input); printf("PASS chmod owner/group/capability/read-only proposals: %lu cases\n",count);
    for (unsigned mode=0;mode<4096;++mode) {
        creds_t cr=actor(3,0);
        vfs_metadata_t value={.mode=VFS_S_IFREG|mode,.gid=43};
        uint16_t want=VFS_S_IFREG|(mode & ~04000u);
        want &= ~02000u;
        assert(permission_content_mode(&value,&cr)==want);
        cr.egid=43;
        want=VFS_S_IFREG|(mode & ~04000u);
        if (mode & 0010u) want &= ~02000u;
        assert(permission_content_mode(&value,&cr)==want);
        cr.cap_effective=CAP_FSETID;
        assert(permission_content_mode(&value,&cr)==(VFS_S_IFREG|mode));
    }
    {
        vfs_metadata_t old={.mode=VFS_S_IFREG|06755,.uid=1001,.gid=1002},next;
        creds_t cr=actor(3,0); creds_id_change_t keep={.keep=true};
        assert(!permission_chown_value(&old,keep,keep,&cr,&next) && next.mode==(VFS_S_IFREG|0755));
        assert(permission_chown_value(&old,(creds_id_change_t){.value=42},keep,&cr,&next)==-VFS_EPERM);
        cr.groups[0]=43;cr.ngroups=1;
        assert(!permission_chown_value(&old,keep,(creds_id_change_t){.value=43},&cr,&next));
        assert(next.gid==43 && next.mode==(VFS_S_IFREG|0755));
        cr.cap_effective=CAP_CHOWN|CAP_FSETID; old.uid=42;
        assert(!permission_chown_value(&old,(creds_id_change_t){.value=UINT32_MAX},keep,&cr,&next));
        assert(next.uid==UINT32_MAX && next.mode==(VFS_S_IFREG|0755));
        old.mode=VFS_S_IFDIR|06755;
        assert(!permission_chown_value(&old,(creds_id_change_t){.value=UINT32_MAX},keep,&cr,&next));
        assert(next.mode==old.mode);
        old.mode=VFS_S_IFREG|06644;
        assert(!permission_chown_value(&old,(creds_id_change_t){.value=UINT32_MAX},keep,&cr,&next));
        assert(next.mode==(VFS_S_IFREG|02644));
    }

    vfs_metadata_t dir={.mode=VFS_S_IFDIR|01777,.uid=42,.gid=43};
    vfs_metadata_t victim={.mode=VFS_S_IFREG|0600,.uid=44,.gid=43};
    creds_t cr=actor(3,0);
    assert(permission_delete_value(&dir,&victim,&cr)==-VFS_EPERM);
    cr.cap_effective=CAP_DAC_OVERRIDE;
    assert(permission_delete_value(&dir,&victim,&cr)==-VFS_EPERM);
    cr.cap_effective=CAP_FOWNER; assert(!permission_delete_value(&dir,&victim,&cr));
    dir.mode=VFS_S_IFDIR|01000;
    assert(permission_delete_value(&dir,&victim,&cr)==-VFS_EACCES);
    cr.cap_effective=CAP_FOWNER|CAP_DAC_OVERRIDE; assert(!permission_delete_value(&dir,&victim,&cr));
    dir.mnt_flags=VFS_MNT_RDONLY; assert(permission_delete_value(&dir,&victim,&cr)==-VFS_EROFS);
    dir.mnt_flags=0; dir.mode=VFS_S_IFDIR|01777; cr=actor(0,0);
    assert(!permission_delete_value(&dir,&victim,&cr)); cr.euid=44;
    assert(!permission_delete_value(&dir,&victim,&cr));
    victim.mode=VFS_S_IFCHR; victim.mnt_flags=VFS_MNT_RDONLY; cr=actor(3,CAP_DAC_OVERRIDE);
    assert(!permission_access_value(&victim,VFS_MAY_WRITE,&cr));
    victim.mode=VFS_S_IFBLK; assert(!permission_access_value(&victim,VFS_MAY_WRITE,&cr));
    victim.mode=0120000; assert(permission_access_value(&victim,VFS_MAY_WRITE,&cr)==-VFS_EROFS);
    assert(permission_access_value(&dir,8,&cr)==-VFS_EINVAL);
    assert(!permission_access_value(&dir,0,&cr));
    cr.ngroups=17; assert(permission_access_value(&dir,1,&cr)==-VFS_EINVAL);
    assert(!permission_group_member(&cr,43)); cr=actor(3,0);
    cr.groups[15]=43; cr.ngroups=16; assert(permission_group_member(&cr,43));
    cr.ngroups=15; assert(!permission_group_member(&cr,43));
    cr=actor(3,0);
    vfs_create_attrs_t out; memset(&out,0xa5,sizeof(out));
    vfs_create_attrs_t before; memcpy(&before,&out,sizeof(out));
    assert(permission_create_value(&dir,VFS_STREAM,0777,&cr,&out)==-VFS_EINVAL);
    assert(!memcmp(&before,&out,sizeof(out)));
    victim.mode=VFS_S_IFREG;
    assert(permission_create_value(&victim,VFS_FILE,0777,&cr,&out)==-VFS_ENOTDIR);
    assert(!memcmp(&before,&out,sizeof(out)));
    puts("PASS sticky ownership/capability, device policy, hostile value bounds and failure publication");
    return 0;
}
