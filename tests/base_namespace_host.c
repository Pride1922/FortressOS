#define TEST_PERMISSIONS_ENFORCEMENT
#define main retained_permission_main
#include "perm_fs_host.c"
#undef main
void kbd_dev_init(vfs_node_t *directory){(void)directory;}
static vfs_node_t tree[24];static unsigned used;
static vfs_node_t *node(vfs_node_t *parent,const char *path,bool directory,const char *data){
    vfs_node_t *n=&tree[used++];assert(used<24);memset(n,0,sizeof *n);
    memcpy(n->path,path,strlen(path)+1);const char *name=path;
    for(const char *p=path;*p;p++)if(*p=='/'&&p[1])name=p+1;
    memcpy(n->name,name,strlen(name)+1);
    n->type=directory?VFS_DIRECTORY:VFS_FILE;n->mode=(directory?VFS_S_IFDIR|0755:VFS_S_IFREG|0644);n->parent=parent;
    n->data=data;n->size=data?strlen(data):0;
    if(parent){n->next=parent->children;parent->children=n;}return n;
}
int main(void){
    vfs_init();creds_t root;creds_init_root(&root);int error;
    static const char base[]="selected-base",old[]="stale-base",config[]="saved-setting",app[]="installed-app";
    const char *directories[]={"/bin","/sbin","/lib","/base","/docs","/usr","/usr/share","/usr/share/wallpapers","/usr/share/licenses","/etc","/etc/ssl"};
    for(unsigned i=0;i<sizeof directories/sizeof directories[0];i++){
        vfs_node_t *n=vfs_create_node_kernel(directories[i],VFS_DIRECTORY,0,NULL);assert(n);n->mode=VFS_S_IFDIR|0755;n->mnt_flags=VFS_MNT_RDONLY;
    }
    vfs_node_t *shell=vfs_create_node_kernel("/bin/shell",VFS_FILE,sizeof base,base);assert(shell);shell->mode=VFS_S_IFREG|0755;shell->mnt_flags=VFS_MNT_RDONLY;
    vfs_node_t *zone=vfs_create_node_kernel("/usr/share/zoneinfo.tzdb",VFS_FILE,sizeof base,base);assert(zone);zone->mnt_flags=VFS_MNT_RDONLY;
    vfs_node_t *installed=node(NULL,"/",true,NULL),*bin=node(installed,"/bin",true,NULL),*etc=node(installed,"/etc",true,NULL);
    node(bin,"/bin/shell",false,old);node(bin,"/bin/removed",false,old);
    vfs_node_t *setting=node(etc,"/etc/desktop.conf",false,config);
    vfs_node_t *usr=node(installed,"/usr",true,NULL),*share=node(usr,"/usr/share",true,NULL),*ub=node(usr,"/usr/bin",true,NULL);
    vfs_node_t *application=node(ub,"/usr/bin/app",false,app);node(share,"/usr/share/zoneinfo.tzdb",false,old);
    vfs_node_t *pkg=node(share,"/usr/share/pkg",true,NULL);node(pkg,"/usr/share/pkg/app",false,app);
    node(installed,"/home",true,NULL);
    assert(!vfs_boot_enable_base());assert(!vfs_boot_publish_root(installed));
    vfs_node_t *n=vfs_lookup_creds("/bin/shell",&root,&error);assert(n==shell&&n->data==base);vfs_node_put(n);
    assert(!vfs_lookup_creds("/bin/removed",&root,&error)&&error==-VFS_ENOENT);
    n=vfs_lookup_creds("/usr/share/zoneinfo.tzdb",&root,&error);assert(n==zone);vfs_node_put(n);
    n=vfs_lookup_creds("/etc/desktop.conf",&root,&error);assert(n==setting&&n->data==config);vfs_node_put(n);
    n=vfs_lookup_creds("/usr/bin/app",&root,&error);assert(n==application);vfs_node_put(n);
    n=vfs_lookup_creds("/bin/..",&root,&error);assert(n==installed);vfs_node_put(n);
    n=vfs_lookup_creds("/usr/share/licenses/..",&root,&error);assert(n==share);vfs_node_put(n);
    vfs_dirent_t item;unsigned bases=0,libs=0;
    for(unsigned i=0;i<64;i++){int r=vfs_readdir(installed,i,&item);assert(r>=0);if(!r)break;if(!strcmp(item.name,"base"))bases++;if(!strcmp(item.name,"lib"))libs++;}
    assert(bases==1&&libs==1);
    assert(vfs_rename_creds("/bin","/old-bin",&root)==-VFS_EPERM);
    assert(vfs_unlink_creds("/usr/share/zoneinfo.tzdb",&root)==-VFS_EPERM);
    vfs_boot_seal_namespace();assert(vfs_boot_enable_base()==-VFS_EPERM);
    puts("PASS actual VFS base namespace: selected binaries/assets, removed-file hiding, mutable config/apps, parent traversal, listing of new routes, mutation refusal and sealed admission");
}
