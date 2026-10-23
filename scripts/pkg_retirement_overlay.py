"""Copied-source retirement/account preview integration; production untouched."""
from test_pkg_service_integration import function

def apply_retirement(work,replace,generated):
    p='tests/pkg_account_durable.inc'
    replace(p,'e4_u16(ad_state+20)!=3','e4_u16(ad_state+20)!=(xr_active?4:3)')
    replace(p,'assert(n<=PE_MAX&&rn==760);','assert(n<=PE_MAX&&rn==1568);')
    replace(p,'ng_policy_load(r+384);free(p);free(r);ad_enabled=true;',
            'ng_policy_load(r+384);xr_policy_load(r,rn);free(p);free(r);ad_enabled=true;')
    replace(p,'static int ad_activate(struct ext4_engine *e,uint32_t ino,uint32_t parent){',
            'static int ad_activate(struct ext4_engine *e,uint32_t ino,uint32_t parent){\n    if(xr_active&&xr_typed)return xr_activate(e,ino,parent);')
    replace(p,'static int ad_retire(struct ext4_engine *e,uint32_t ino){',
            'static int ad_retire(struct ext4_engine *e,uint32_t ino){\n    if(xr_active&&xr_typed)return xr_retire(e,ino);')
    replace('src/fs/ext4_mutate.inc','static int e4_plan_begin(ext4_engine_t *e) {',
            'static int e4_plan_begin(ext4_engine_t *e) {\n    if(e&&xr_active&&xr_typed){int check=xr_preflight(e);if(check)return check;}')
    p='tests/pkg_account_service_durable.inc'
    replace(p,'    if(!o.active)return e->orphan_count?-VFS_EIO:0;',
            '    if(!o.active)return xr_active?0:(e->orphan_count?-VFS_EIO:0);')
    replace(p,'static int e4_semantic_commit(struct ext4_engine *e){','static int xr_legacy_commit(struct ext4_engine *e){')
    replace(p,'static int sv_begin(void *p){','static int sv_begin(void *p){\n    if(xr_active)return -VFS_EAGAIN;')
    p='tests/pkg_mount_admission.inc'
    replace(p,' ad_hash("Fortress private account/grant v3",sizeof("Fortress private account/grant v3")-1,ad_body+312);',
            ' ad_hash("Fortress private retirement/account v4",sizeof("Fortress private retirement/account v4")-1,ad_body+312);')
    replace(p,'static int ng_extra(void){','static int ng_extra(void){\n if(xr_active){for(unsigned i=436;i<440;i++)if(ad_state[i])return -VFS_EIO;for(unsigned i=624;i<992;i++)if(ad_state[i])return -VFS_EIO;return 0;}')
    replace(p,'static int ng_gate(ext4_engine_t *e,bool planned){',
            'static int ng_gate(ext4_engine_t *e,bool planned){\n if(xr_active)return xr_gate(e,planned);')
    replace(p,'static int ng_fence(ext4_engine_t *e){',
            'static int ng_fence(ext4_engine_t *e){\n if(xr_active)return xr_fence(e);')
    replace(p,'static void ng_seed_files(ext4_mount_t *fs){',
            'static void ng_seed_files(ext4_mount_t *fs){\n xr_seed(fs);')
    replace(p,'int main(int argc,char **argv){','int xr_previous_main(int argc,char **argv){')
    replace('src/fs/ext4_mount_journal.inc',
            '    r=ext4_engine_orphan_recover(engine);if (r) goto fail;',
            '    xr_typed=xr_active;r=ext4_engine_orphan_recover(engine);xr_typed=false;if (r) goto fail;')
    replace('tests/pkg_account_durable.inc','ad_initialize(fs);assert(!ext4_freeze_and_sync(fs));','ad_initialize(fs);xr_export(fs,prefix);assert(!ext4_freeze_and_sync(fs));')
    replace('tests/pkg_mount_admission.inc','static void ng_preview(ext4_engine_t *e){','static void ng_preview(ext4_engine_t *e){\n if(xr_active){xr_preview_report(e);return;}')
    replace('src/fs/ext4_mount_journal.inc',
        '    r=e4_open_journal_orphans_owned(dev,true,&engine,true,true);if (r) goto fail;',
        '    r=e4_open_journal_orphans_owned(dev,true,&engine,true,true);if (r) goto fail;\n    r=xr_reopened(engine);if(r)goto fail;')
    replace('tests/pkg_mount_admission.inc','  allow|=v->grant.phase==PG_IDLE&&v->installed&&!strcmp(ent.name,"installed.fpk");',
        '  if(xr_active)allow|=xr_name_allowed(ent.name);\n  allow|=v->grant.phase==PG_IDLE&&v->installed&&!strcmp(ent.name,"installed.fpk");')
    # Reuse real constructor preview; refresh policy/authority before activation,
    # rather than relying only on the earlier preview's borrowed buffers.
    text=generated.read_text()
    declarations='\nstatic bool xr_active;\nstatic int xr_gate(struct ext4_engine *,bool);\nstatic int xr_fence(struct ext4_engine *);\nstatic int xr_reopened(struct ext4_engine *);\nstatic void xr_policy_load(const uint8_t *,size_t);\nstatic void xr_seed(struct ext4_mount *);\nstatic void xr_export(struct ext4_mount *,const char *);\nstatic void xr_preview_report(struct ext4_engine *);\nstatic bool xr_typed;\nstatic int xr_activate(struct ext4_engine *,unsigned,unsigned);\nstatic int xr_retire(struct ext4_engine *,unsigned);\nstatic int xr_preflight(struct ext4_engine *);\nstatic bool xr_name_allowed(const char *);\n'
    needle='struct ext4_engine;struct ext4_mount;struct vfs_node;'
    assert text.count(needle)==1
    generated.write_text(text.replace(needle,needle+declarations)+'\n#include "pkg_retirement_integrated.inc"\n')
