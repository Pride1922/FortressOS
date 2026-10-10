#define main baseline_ext2_main
#include "ext2_host.c"
#undef main
static void lifetime_checks(void) {
 int e;vfs_metadata_t m;char data[4];
 vfs_node_t *root=vfs_lookup_ref("/mnt",&e);assert(root && !e);
 ext2_fs_t *fs=((ext2_inode_t *)root->fs_private)->fs;uint32_t base=fs->nodes;
 size_t before=live;io_failure=true;vfs_dirent_t failed_dent;
 assert(ext_readdir(root,0,&failed_dent)==-VFS_EIO);
 assert(!ext_lookup_ref(root,"missing",&e) && e==-VFS_EIO && live==before);
 io_failure=false;fail_after=0;
 assert(!ext_lookup_ref(root,"lost+found",&e) && e==-VFS_ENOMEM && live==before);
 fail_after=-1;
 vfs_node_t *held=vfs_create_ref("/mnt/held",VFS_FILE,&e);assert(held && !e);
 file_t *f=vfs_open("/mnt/held",VFS_O_RDWR);assert(f && vfs_write(f,"old",3)==3);
 size_t w=writes,q=flushes;
 assert(vfs_unlink("/mnt/held")==-VFS_EOPNOTSUPP && writes==w && flushes==q);
 vfs_node_t *source=vfs_create_ref("/mnt/source",VFS_FILE,&e);assert(source);
 w=writes;q=flushes;
 assert(vfs_rename("/mnt/source","/mnt/held")==-VFS_EOPNOTSUPP && writes==w && flushes==q);
 assert(!vfs_rename("/mnt/held","/mnt/moved"));f->offset=0;
 assert(vfs_read(f,data,3)==3 && !memcmp(data,"old",3));assert(!strcmp(held->path,"/mnt/moved"));
 assert(!vfs_close(f));assert(!vfs_unlink("/mnt/moved"));
 assert(vfs_metadata(held,&m)==-VFS_ENOENT && ext_read(held,0,data,3)==-VFS_ENOENT);
 assert(ext_truncate(held,0)==-VFS_ENOENT && ext_can_write(held)==-VFS_ENOENT);
 vfs_node_t *replacement=vfs_create_ref("/mnt/moved",VFS_FILE,&e);assert(replacement && replacement!=held);
 assert(!vfs_metadata(replacement,&m) && !m.size);
 vfs_node_put(held);vfs_node_put(replacement);assert(!vfs_unlink("/mnt/moved"));
 vfs_node_put(source);assert(!vfs_unlink("/mnt/source"));assert(fs->nodes==base);
 vfs_node_t *dir=vfs_create_ref("/mnt/d",VFS_DIRECTORY,&e);assert(dir);
 vfs_node_t *child=vfs_create_ref("/mnt/d/c",VFS_FILE,&e);assert(child);
 assert(!vfs_rename("/mnt/d","/mnt/e"));assert(!strcmp(child->path,"/mnt/e/c"));
 vfs_node_put(child);assert(!vfs_unlink("/mnt/e/c"));assert(!vfs_unlink("/mnt/e"));
 vfs_dirent_t dent;assert(ext_readdir(dir,0,&dent)==-VFS_ENOENT);
 assert(!ext_create_ref(dir,"late",VFS_FILE,&e) && e==-VFS_ENOENT);
 vfs_node_put(dir);assert(fs->nodes==base);vfs_node_put(root);
 puts("PASS EXT2 lifetime: owned retirement/reuse, open unlink/replacement zero writes, rename open/source descendants");
}
int main(int argc,char **argv) {
 assert(argc==3);FILE *fp=fopen(argv[1],"rb");assert(fp);assert(!fseek(fp,0,SEEK_END));disk_size=ftell(fp);rewind(fp);
 pending_disk=malloc(disk_size);durable_disk=malloc(disk_size);assert(pending_disk && durable_disk && fread(disk,1,disk_size,fp)==disk_size);fclose(fp);
 memcpy(durable_disk,disk,disk_size);reset();block_dev_t dev={.sector_size=512,.sector_count=disk_size/512,.read_sector=read_sector,.write_sector=write_sector,.flush=flush_sector};
 for (unsigned creator=1;creator<=5;creator++) {
   uint32_t old=u32(disk+1024+72);put32(disk+1024+72,creator);
   size_t w=writes,q=flushes,allocated=live;
   assert(!ext2_mount(&dev,"/mnt") && !ext2_mount_rw(&dev,"/mnt"));
   assert(writes==w && flushes==q && live==allocated && !vfs_lookup("/mnt"));
   put32(disk+1024+72,old);
 }
 puts("PASS EXT2 creator-OS ownership admission: foreign layouts RO/RW zero writes/flushes/allocation/publication");
 assert(ext2_mount_rw(&dev,"/mnt"));lifetime_checks();
 for(unsigned mode=0;mode<=07777;mode++) {
   vfs_create_attrs_t a={.mode=mode,.uid=0x12345678,.gid=0x87654321};int e;
   vfs_node_t *n=vfs_create_attrs_ref("/mnt/mode",VFS_FILE,&a,&e);assert(n && !e);
   vfs_metadata_t m;assert(!vfs_metadata(n,&m) && m.mode==(VFS_S_IFREG|mode) && m.uid==a.uid && m.gid==a.gid);
   vfs_node_put(n);if(mode!=07777)assert(!vfs_unlink("/mnt/mode"));
 }
 assert(ext2_sync_all());fp=fopen(argv[2],"wbx");assert(fp && fwrite(durable_disk,1,disk_size,fp)==disk_size);fclose(fp);
 while(live) { free(allocations[--live]); }free(pending_disk);free(durable_disk);
 puts("PASS EXT2 metadata: all 4096 modes, full32 uid/gid, retained final inode");return 0;
}
