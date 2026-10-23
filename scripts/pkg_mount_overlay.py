"""Private mount profile overlay on top of account service copied sources."""
from test_pkg_service_integration import function

def apply_mount(work,replace):
 replace('src/kernel/accounts.c','int r=ext4_root_account_headroom(storage.context);if(!r)r=sv_begin(storage.context);',
         'int r=ng_headroom(storage.context);if(!r)r=sv_begin(storage.context);')
 p='tests/pkg_account_durable.inc'
 replace(p,'e4_u16(ad_state+20)!=2','e4_u16(ad_state+20)!=3')
 replace(p,'e4_p16(ad_state+20,2);','e4_p16(ad_state+20,3);')
 old=function((work/p).read_text(),'static int ad_pair(ext4_engine_t *e,bool planned,pe_origin *origin){')
 assert old.endswith('    return 0;\n}')
 replace(p,old,old[:-len('    return 0;\n}')]+ '    return ng_gate(e,planned);\n}')
 replace(p,'    uint64_t seq=ad_u64(ad_state+24);if(seq>=UINT64_MAX-1)return -VFS_EFBIG;',
         '    int nr=ng_stamp(e);if(nr)return nr;\n    uint64_t seq=ad_u64(ad_state+24);if(seq>=UINT64_MAX-1)return -VFS_EFBIG;')
 replace(p,'    sv_seed(fs);','    sv_seed(fs);ng_seed_files(fs);')
 replace(p,'    memset(ad_slot,0,1024);','    ng_seed_records(fs);\n    memset(ad_slot,0,1024);')
 replace(p,'    sv_seed_state(fs);ad_hash(ad_slot,1024,ad_state+112);',
         '    sv_seed_state(fs);assert(!ng_stamp(fs->engine));ad_hash(ad_slot,1024,ad_state+112);')
 replace(p,'assert(n<=PE_MAX&&rn==384);','assert(n<=PE_MAX&&rn==760);')
 replace(p,'assert(!pe_verify(ad_descriptor,n,&policy,&ad_en));free(p);free(r);ad_enabled=true;',
         'assert(!pe_verify(ad_descriptor,n,&policy,&ad_en));ng_policy_load(r+384);free(p);free(r);ad_enabled=true;')
 replace(p,'    ad_previews++;int r=ad_validate(e);',
         '    ad_previews++;int r=ad_validate(e);if(!r)ng_preview(e);')
 p='tests/pkg_account_service_durable.inc'
 replace(p,'    for(unsigned i=436;i<992;i++)if(ad_state[i])return -VFS_EIO;', '    if(ng_extra())return -VFS_EIO;')
 replace(p,'int main(int argc,char **argv){','int ng_service_main(int argc,char **argv){')
 replace('src/fs/ext4_transaction.inc','    if (!r) r=jbd2_writer_checkpoint(e->writer);',
         '    if(!r&&ng_pause){ng_pause=false;r=-VFS_EIO;}\n    if (!r) r=jbd2_writer_checkpoint(e->writer);')
 replace(p,'    int r=sv_primitive_commit(e);','    int guard=ng_fence(e);if(guard)return guard;\n    int r=sv_primitive_commit(e);')
 replace(p,'            assert(enabled&&!writer&&generation==ad_u64(ad_state+144));creds_t root;creds_init_root(&root);','            ng_guard_tests(sv_fs);\n            assert(enabled&&!writer&&generation==ad_u64(ad_state+144));creds_t root;creds_init_root(&root);')
