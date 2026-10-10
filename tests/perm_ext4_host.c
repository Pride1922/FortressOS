#define main baseline_ext4_main
#include "ext4_mount_host.c"
#undef main
static int creation_cuts(const char *source,const char *prefix) {
 size_t n;uint8_t *initial=load(source,&n);
 ext4_fault_disk_t d={.stable=malloc(n),.volatile_bytes=malloc(n),.bytes=n,.cut=-1};assert(d.stable && d.volatile_bytes);
 block_dev_t dev={.sector_size=512,.sector_count=n/512,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
 vfs_create_attrs_t a={.mode=06751,.uid=0x12345678,.gid=0x87654321};int e;ext4_mount_t *fs;
 reset(&d,initial);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));d.events=0;
 vfs_node_t *node=vfs_create_attrs_ref("/mnt/attributes",VFS_FILE,&a,&e);assert(node && !e);vfs_node_put(node);
 size_t events=d.events,count=0;bool saved[2]={false,false};
 for(unsigned persistence=0;persistence<4;persistence++) for(size_t cut=0;cut<events;cut++) for(unsigned after=0;after<2;after++) {
  reset(&d,initial);assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
  d.events=0;d.cut=(long)cut;d.after=after;d.persistence=persistence;
  node=vfs_create_attrs_ref("/mnt/attributes",VFS_FILE,&a,&e);if(node)vfs_node_put(node);
  assert(d.offline);teardown();ext4_fault_restart(&d);vfs_init();
  assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
  node=vfs_lookup_ref("/mnt/attributes",&e);unsigned state=node!=NULL;
  if(node) {vfs_metadata_t m;assert(!vfs_metadata(node,&m));
   assert(m.mode==(VFS_S_IFREG|a.mode) && m.uid==a.uid && m.gid==a.gid && !m.size);vfs_node_put(node);
  } else assert(e==-VFS_ENOENT);
  assert(!ext4_freeze_and_sync(fs));clean_check(fs);
  if(!saved[state]) {save(prefix,state ? "new" : "old",d.stable,n);saved[state]=true;}
  count++;
 }
 assert(saved[0] && saved[1]);teardown();free(initial);free(d.stable);free(d.volatile_bytes);
 printf("PASS EXT4 attributes old-or-new: %zu cuts, four atomic persistence models, complete mode/UID/GID\n",count);return 0;
}
int main(int argc,char **argv) {
 if(argc==4 && !strcmp(argv[3],"--cuts"))return creation_cuts(argv[1],argv[2]);
 assert(argc==3);size_t n;uint8_t *initial=load(argv[1],&n);
 ext4_fault_disk_t d={.stable=malloc(n),.volatile_bytes=malloc(n),.bytes=n,.cut=-1};assert(d.stable && d.volatile_bytes);
 block_dev_t dev={.sector_size=512,.sector_count=n/512,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
 reset(&d,initial);ext4_mount_t *fs=NULL;assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
 for(unsigned mode=0;mode<=07777;mode++) {
   vfs_create_attrs_t a={.mode=mode,.uid=0x12345678,.gid=0x87654321};int e;
   vfs_node_t *node=vfs_create_attrs_ref("/mnt/mode",VFS_FILE,&a,&e);assert(node && !e);
   vfs_metadata_t m;assert(!vfs_metadata(node,&m) && m.mode==(VFS_S_IFREG|mode) && m.uid==a.uid && m.gid==a.gid);
   vfs_node_put(node);if(mode!=07777)assert(!vfs_unlink("/mnt/mode"));
 }
 assert(!ext4_freeze_and_sync(fs));clean_check(fs);
 FILE *fp=fopen(argv[2],"wbx");assert(fp && fwrite(d.stable,1,n,fp)==n);fclose(fp);
 teardown();free(initial);free(d.stable);free(d.volatile_bytes);
 puts("PASS journaled EXT4 metadata: all 4096 modes, full32 uid/gid, checksum reader, bounded node reuse");return 0;
}
