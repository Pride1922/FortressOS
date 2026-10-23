"""Exact copied-source service/redo integration. Never edits production files."""
from test_pkg_service_integration import function


def apply(work, replace):
    p='tests/ext4_mount_host.c'
    replace(p,'static void teardown(void) {','static void sv_previous_teardown(void) {')
    p='tests/pkg_account_durable.inc'
    replace(p,'    uint32_t ino=2;const char *p=path+5;',
            '    if(!strcmp(path,"/mnt")){*out=2;return 0;}\n    uint32_t ino=2;const char *p=path+5;')
    replace(p,'int main(int argc,char **argv){','int sv_primitive_previous_main(int argc,char **argv){')
    replace(p,'static int e4_semantic_commit(struct ext4_engine *e){','static int sv_primitive_commit(struct ext4_engine *e){')
    replace(p,'e4_u16(ad_state+20)!=1','e4_u16(ad_state+20)!=2')
    replace(p,'    for(unsigned i=144;i<992;i++)if(ad_state[i])return -VFS_EIO;',
            '    if(sv_state_extra())return -VFS_EIO;')
    replace(p,'origin->phase||gen<2','origin->phase!=e4_u16(ad_state+160)||gen<2')
    replace(p,'ad_u64(ad_state+24)>UINT64_MAX-5))', 'ad_u64(ad_state+24)>UINT64_MAX-256))')
    replace(p,'UINT64_MAX-4','UINT64_MAX-128')
    replace(p,'    uint32_t actual;pe_origin o;int r=ad_pair(e,false,&o);',
            '    unsigned role=sv_role;if(role>=ACCOUNT_ROLE_COUNT)return -VFS_EROFS;\n'
            '    bool priv=(role>=3&&role<=5)||role>=9;\n'
            '    uint32_t actual;pe_origin o;int r=ad_pair(e,false,&o);')
    replace(p,'ad_path(e->fs,"/mnt/etc/shadow",&actual)||actual!=ino||parent!=e4_u32(ad_locator(9)+4)',
            'ad_path(e->fs,sv_path(role),&actual)||actual!=ino||parent!=e4_u32(ad_locator(priv?13:9)+4)')
    replace(p,'in.full_mode!=(0x8000|0600)||in.size>8192',
            'in.full_mode!=(0x8000|e4_ac_mode(role))||in.size>8192')
    replace(p,'    ad_empty();e4_p16(ad_slot+22,1);e4_p16(ad_slot+112,2); /* ACCOUNT_SHADOW; pre-intent */',
            '    if(role==ACCOUNT_PENDING){if(e4_u16(ad_state+160)!=1)return -VFS_EIO;e4_p16(ad_state+160,2);}\n'
            '    ad_empty();e4_p16(ad_slot+22,1);e4_p16(ad_slot+112,(uint16_t)role);e4_p16(ad_slot+114,e4_u16(ad_state+160));')
    replace(p,'e4_p32(ad_slot+128,e4_u32(ad_locator(9)+8))','e4_p32(ad_slot+128,e4_u32(ad_locator(priv?13:9)+8))')
    replace(p,'    e4_p32(ad_slot+132,0600);','    e4_p32(ad_slot+132,e4_ac_mode(role));')
    text=(work/p).read_text();old=function(text,'static int ad_validate(struct ext4_engine *e){')
    cut=old.index('    pe_origin o;if(ad_pair(e,false,&o))')
    identity=old[:cut].replace('ad_validate','sv_locators',1)+'    return 0;\n}'
    replace(p,old,identity)
    replace(p,'for(unsigned i=0;i<sizeof(dirs)/sizeof(*dirs);i++)assert(!vfs_mkdir(dirs[i],0700));',
            'for(unsigned i=0;i<sizeof(dirs)/sizeof(*dirs);i++)assert(!vfs_mkdir(dirs[i],i>=2&&i<=5?0755:0700));')
    replace(p,'    memset(ad_body,0,sizeof ad_body);','    sv_seed(fs);\n    memset(ad_body,0,sizeof ad_body);')
    replace(p,'e4_p16(ad_state+20,1);','e4_p16(ad_state+20,2);')
    replace(p,'    ad_hash(ad_slot,1024,ad_state+112);ad_seal(ad_state);\n    ad_write(ad_paths[4]',
            '    sv_seed_state(fs);ad_hash(ad_slot,1024,ad_state+112);ad_seal(ad_state);\n    ad_write(ad_paths[4]')
    p='src/fs/ext4_namespace.inc'
    replace(p,'static int e4_namespace_end(ext4_engine_t *e,int r) {',
            'static int sv_namespace_core_end(ext4_engine_t *e,int r) {')
    p='src/fs/ext4_write.inc'
    replace(p,'    if (!r) n->opens++;','    if(!r&&sv_pin_deny(node))r=-VFS_EAGAIN;\n    if (!r) n->opens++;')
    replace('src/fs/ext4_permissions.inc','    if (!r && node->open) n->opens++;',
            '    if(!r&&sv_pin_deny(node))r=-VFS_EAGAIN;\n    if (!r && node->open) n->opens++;')
    p='src/kernel/accounts.c'
    replace(p,'storage.validate=coherent;account_workspace_t *w=workspace();',
            'storage.validate=coherent;int sr=sv_boot_bind(fs,&storage);if(sr)return sr;account_workspace_t *w=workspace();')
    replace(p,'r=account_redo_recover(&storage,w);release_workspace(w);enabled=!r;return r;',
            'r=account_redo_recover(&storage,w);if(!r)r=sv_boot_end(fs);release_workspace(w);enabled=!r;sv_relax();return r;')
    replace(p,'int r=ext4_root_account_headroom(storage.context);',
            'int r=ext4_root_account_headroom(storage.context);if(!r)r=sv_begin(storage.context);')
    replace(p,'else {__atomic_store_n(&writer,0,__ATOMIC_RELEASE);token=0;enabled=!__atomic_load_n(&latched,__ATOMIC_ACQUIRE);}',
            'else if((r=sv_end(storage.context)))result=syscall_from_vfs_error(r);\n'
            '            else {__atomic_store_n(&writer,0,__ATOMIC_RELEASE);token=0;enabled=!__atomic_load_n(&latched,__ATOMIC_ACQUIRE);}')
    replace(p,'            if (!r && generation==UINT64_MAX)',
            '            if(!r)r=sv_issue_pre(storage.context,generation);\n            if (!r && generation==UINT64_MAX)')
    replace(p,'enabled=false;__atomic_add_fetch(&generation,1,__ATOMIC_RELEASE);r=account_redo_update(&storage,w,generation);',
            'enabled=false;__atomic_add_fetch(&generation,1,__ATOMIC_RELEASE);r=sv_issue(storage.context,w,generation);\n'
            '                if(!r)r=account_redo_update(&storage,w,generation);\n                if(!r)r=sv_complete(storage.context);')
    replace(p,'release_workspace(w);leave();return result;','release_workspace(w);leave();sv_relax();return result;')
    old=function((work/p).read_text(),'static void retire_dead(void) {')
    replace(p,old,'''static void retire_dead(void) {
    if(!__atomic_exchange_n(&writer_dead,false,__ATOMIC_ACQUIRE))return;
    enabled=false;__atomic_store_n(&writer,0,__ATOMIC_RELEASE);token=0;
}''')
