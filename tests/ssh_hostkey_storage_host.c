/* Actual mounted EXT4/JBD2 + fixed host-key storage + syscall adapter.
 * Sector-atomic regular-file fault model, not device cache/tear assurance. */
#define TEST_PERMISSIONS_ENFORCEMENT
#define main phase0_main
#include "ext4_mount_host.c"
#undef main
#include <unistd.h>
#include "../src/kernel/creds.c"
#include "../src/fs/permission_values.c"
#include "../src/kernel/hostkey.c"
uint64_t *vmm_get_active_pml4_virt(void) {return (void *)1;}
bool vmm_validate_user_range(uint64_t *p,uintptr_t a,size_t n,bool output) {
    (void)p;(void)output;return a>=4096 && a<0x800000000000ull && n<=0x800000000000ull-a;
}
int64_t syscall_from_vfs_error(int64_t e) {
    switch(-e) {
        case VFS_ENOMEM:return SYSCALL_ENOMEM;case VFS_EINVAL:return SYSCALL_EINVAL;
        case VFS_EEXIST:return SYSCALL_EEXIST;case VFS_ENOENT:return SYSCALL_ENOENT;
        case VFS_EACCES:return SYSCALL_EACCES;case VFS_EAGAIN:return SYSCALL_EAGAIN;
        default:return e ? SYSCALL_EIO : 0;
    }
}
static long cut=-1;static bool cut_write,offline;static unsigned writes,flushes;
static const char *io_mode;
static vfs_node_t *(*original_create)(vfs_node_t *,const char *,vfs_node_type_t,const vfs_create_attrs_t *,int *);
static int64_t (*original_write)(vfs_node_t *,uint64_t *,bool,const void *,size_t);
static vfs_node_t *io_node;
static int64_t injected_write(vfs_node_t *node,uint64_t *offset,bool append,const void *p,size_t n) {
    if(!strcmp(io_mode,"zero")) return 0;
    if(!strcmp(io_mode,"enospc")) return -VFS_ENOSPC;
    return original_write(node,offset,append,p,n>17 ? 17 : n);
}
static vfs_node_t *injected_create(vfs_node_t *dir,const char *name,vfs_node_type_t type,const vfs_create_attrs_t *attrs,int *error) {
    vfs_node_t *node=original_create(dir,name,type,attrs,error);
    if(node) {assert(!node->get(node));io_node=node;original_write=node->write;node->write=injected_write;}
    return node;
}
static pthread_barrier_t start_gate;
typedef struct {ext4_mount_t *fs;const char *key;size_t size;int result;} racer;
static void *race(void *ctx) {
    racer *r=ctx;int s=pthread_barrier_wait(&start_gate);assert(!s || s==PTHREAD_BARRIER_SERIAL_THREAD);
    r->result=ext4_hostkey_publish(r->fs,r->key,r->size);return NULL;
}
static bool image_read(block_dev_t *d,uint64_t l,void *p) {
    return !offline && !fseeko(d->priv,l*d->sector_size,SEEK_SET) && fread(p,1,d->sector_size,d->priv)==d->sector_size;
}
static bool image_write(block_dev_t *d,uint64_t l,const void *p) {
    if(offline) return false;
    if(cut_write && cut==(long)writes++) {offline=true;return false;}
    if(!cut_write) writes++;
    return !fseeko(d->priv,l*d->sector_size,SEEK_SET) && fwrite(p,1,d->sector_size,d->priv)==d->sector_size;
}
static bool image_flush(block_dev_t *d) {
    if(offline) return false;
    if(!cut_write && cut==(long)flushes++) {offline=true;return false;}
    if(cut_write) flushes++;
    return !fflush(d->priv) && !fsync(fileno(d->priv));
}
int main(int argc,char **argv) {
    assert(argc==5 || argc==7);unsigned sector=(unsigned)strtoul(argv[2],NULL,10);
    char key[1024],out[1024];FILE *kf=fopen(argv[3],"rb");assert(kf);size_t length=fread(key,1,sizeof(key),kf);fclose(kf);assert(length && length<1024);
    FILE *f=fopen(argv[1],"r+b");assert(f);
    block_dev_t d={.priv=f,.sector_size=sector,.sector_count=256*1024*1024/sector,.read_sector=image_read,.write_sector=image_write,.flush=image_flush};
    vfs_init();size_t baseline=live;ext4_mount_t *fs=NULL;assert(!ext4_prepare_installer_root(&d,true,&fs,NULL));
    size_t n=999;int r=ext4_hostkey_read(fs,out,sizeof(out),&n);
    if(!strcmp(argv[4],"recover")) {
        assert(r==-VFS_ENOENT || !r);
        if(!r) assert(n==length && !memcmp(key,out,n));
        printf("RECOVER %s\n",r ? "absent" : "valid");
    } else if(!strcmp(argv[4],"reject")) {
        assert(r==-VFS_EACCES || r==-VFS_EINVAL);
        assert(ext4_hostkey_publish(fs,key,length));
    } else {
        assert(r==-VFS_ENOENT && n==999);
        /* No active installed root means no syscall fallback to this detached
         * target or to a live namespace. Check precedence and caller authority. */
        creds_t actor;creds_init_root(&actor);
        assert(hostkey_control(HOSTKEY_READ,(uintptr_t)out,1024,&actor)==SYSCALL_EOPNOTSUPP);
        assert(hostkey_control(HOSTKEY_READ,0,1024,&actor)==SYSCALL_EFAULT);
        actor.euid=1000;assert(hostkey_control(HOSTKEY_READ,0,1024,&actor)==SYSCALL_EPERM);
        assert(hostkey_control(2,0,1024,&actor)==SYSCALL_EINVAL);
        __atomic_store_n(&e4_hostkey_busy,1,__ATOMIC_RELEASE);
        assert(ext4_hostkey_publish(fs,key,length)==-VFS_EAGAIN);
        __atomic_store_n(&e4_hostkey_busy,0,__ATOMIC_RELEASE);
        size_t before=live;fail_alloc=0;
        assert(ext4_hostkey_publish(fs,key,length)==-VFS_ENOMEM);fail_alloc=-1;assert(live==before);
        writes=flushes=0;
        if(argc==7) {cut_write=!strcmp(argv[5],"write");cut=strtol(argv[6],NULL,10);}
        vfs_node_t *io_parents[3]={0};
        bool injected=!strcmp(argv[4],"short") || !strcmp(argv[4],"zero") || !strcmp(argv[4],"enospc");
        if(injected) {
            io_mode=argv[4];assert(!e4_hostkey_parents(fs,true,io_parents));
            original_create=io_parents[2]->create_attrs_ref;io_parents[2]->create_attrs_ref=injected_create;
        }
        if(!strcmp(argv[4],"race")) {
            assert(!pthread_barrier_init(&start_gate,NULL,2));pthread_t threads[2];
            racer racers[2]={{fs,key,length,999},{fs,key,length,999}};
            for(unsigned i=0;i<2;i++) assert(!pthread_create(&threads[i],NULL,race,&racers[i]));
            for(unsigned i=0;i<2;i++) assert(!pthread_join(threads[i],NULL));
            assert((!racers[0].result)!=(!racers[1].result));
            int rejected=racers[0].result ? racers[0].result : racers[1].result;
            assert(rejected==-VFS_EAGAIN || rejected==-VFS_EEXIST);assert(!pthread_barrier_destroy(&start_gate));r=0;
        } else r=ext4_hostkey_publish(fs,key,length);
        if(injected) {
            io_parents[2]->create_attrs_ref=original_create;
            if(io_node) {io_node->write=original_write;vfs_node_put(io_node);io_node=NULL;}
            e4_hostkey_release(io_parents);
            if(strcmp(argv[4],"short")) {
                assert(r==(!strcmp(argv[4],"zero") ? -VFS_EIO : -VFS_ENOSPC));
                assert(ext4_hostkey_read(fs,out,sizeof(out),&n)==-VFS_ENOENT);
                goto finish;
            }
        }
        if(argc==7 && offline) {
            assert(r);printf("CUT writes=%u flushes=%u\n",writes,flushes);
            /* Simulated process/power death: no in-memory mount cleanup. */
            fflush(stdout);_Exit(0);
        }
        assert(!r);
        printf("PUBLISH writes=%u flushes=%u\n",writes,flushes);
        assert(!ext4_hostkey_read(fs,out,sizeof(out),&n));assert(n==length && !memcmp(key,out,n));
        assert(ext4_hostkey_publish(fs,"replacement",11)==-VFS_EEXIST);
        assert(!ext4_hostkey_read(fs,out,sizeof(out),&n));assert(n==length && !memcmp(key,out,n));
        vfs_node_t *parents[3]={0};assert(!e4_hostkey_parents(fs,false,parents));int err;
        vfs_node_t *node=e4_hostkey_lookup(parents[2],HOSTKEY_NAME,&err);assert(node);
        creds_t ordinary={.uid=1000,.euid=1000,.suid=1000,.gid=1000,.egid=1000,.sgid=1000};
        assert(vfs_permission(node,VFS_MAY_READ,&ordinary)==-VFS_EACCES);
        vfs_node_put(node);e4_hostkey_release(parents);
    }
finish:
    assert(!ext4_freeze_and_sync(fs));assert(!ext4_discard_prepared_root(fs));assert(live==baseline);fclose(f);
    puts("PASS actual EXT4 host identity and reclamation");
}
