/* Private actual EXT4 refinement of the bounded package mount profile. */
#include "pkg_mount_candidate.h"
static fpk_catalog_policy ng_policy;
static uint8_t ng_platform[32];static uint64_t ng_base;
static const char *ng_dir;static unsigned ng_phase,ng_publication;
static bool ng_required,ng_pause,ng_typed;static unsigned ng_newer;
static uint8_t ng_m[512],ng_l[1024],ng_t[8192],ng_s[1024],ng_tr[1024],ng_p[1024],ng_n[1024],ng_art[8192];
static const char *ng_names[]={"/mnt/pkg/transaction.v1/artifact.fpk","/mnt/pkg/transaction.v1/activation","/mnt/pkg/transaction.v1/next.v1","/mnt/pkg/transaction.v1/pending.v1"};
static unsigned ng_role[]={1,17,18,19};
static void ng_config(void){ng_dir=getenv("PKG_MOUNT_INPUT");assert(ng_dir);const char *v=getenv("PKG_MOUNT_PHASE");ng_phase=v?(unsigned)strtoul(v,NULL,10):0;v=getenv("PKG_MOUNT_PUBLICATION");ng_publication=v?(unsigned)strtoul(v,NULL,10):0;assert(ng_phase<=3&&ng_publication<=2);}
static uint8_t *ng_input(const char *name,size_t *n){char path[768];assert(snprintf(path,sizeof path,"%s/%s",ng_dir,name)<(int)sizeof path);return load(path,n);}
static void ng_policy_load(const uint8_t *p){
 memcpy(ng_policy.repository,p,16);ng_policy.root_version=ad_u64(p+16);ng_policy.revision_floor=ad_u64(p+24);ng_policy.now=ad_u64(p+32);memcpy(ng_policy.floor_sha256,p+40,32);
 ng_policy.threshold=e4_u32(p+72);ng_policy.key_count=e4_u32(p+76);memcpy(ng_policy.keys,p+80,256);memcpy(ng_platform,p+336,32);ng_base=ad_u64(p+368);ng_required=true;
}
static int ng_block(ext4_engine_t *e,uint32_t block,void *bytes,bool planned){
 if(planned)for(unsigned i=0;i<e->images;i++)if(e->image[i].block==block){memcpy(bytes,e->image[i].bytes,e->fs->bs);return 0;}
 return e4_bytes(e->fs,(uint64_t)block*e->fs->bs,bytes,e->fs->bs)?0:-VFS_EIO;
}
static int ng_file(ext4_engine_t *e,const char *path,void *bytes,size_t cap,size_t *length,unsigned mode,bool planned,uint32_t *id,uint64_t *allocated){
 ext4_mount_t *fs=e->fs;uint32_t ino;int r=ad_path(fs,path,&ino);if(r)return r;e4_inode_t in;
 if(e4_inode(fs,ino,&in)||in.size>cap||in.uid||in.gid||in.full_mode!=(VFS_S_IFREG|mode))return -VFS_EIO;
 uint32_t group=(ino-1)/fs->ipg,bit=(ino-1)%fs->ipg;uint8_t raw[256];
 if(!e4_bytes(fs,(uint64_t)e4_u32(fs->gd[group].raw+8)*fs->bs+bit*256,raw,256)||e4_u16(raw+26)!=1)return -VFS_EIO;
 fs->rw_ino=0;if(e4_mapping(&in))return -VFS_EIO;
 uint32_t blocks[8];unsigned n=(unsigned)((in.size+fs->bs-1)/fs->bs);if(n>8||fs->physical_count!=in.map->count)return -VFS_EIO;
 uint64_t logical=0,total=0;
 for(unsigned i=0;i<in.map->count;i++){e4_extent_t x=in.map->entries[i];if(x.unwritten||x.logical!=logical)return -VFS_EIO;logical+=x.len;total+=x.len;}
 if(logical!=n||total*(fs->bs/512)!=e4_u32(raw+28))return -VFS_EIO;
 for(unsigned i=0;i<n;i++){const e4_extent_t *x=e4_find(&in,i);if(!x)return -VFS_EIO;blocks[i]=x->physical+i-x->logical;}
 uint8_t *buf=kmalloc(fs->bs);if(!buf)return -VFS_ENOMEM;
 for(unsigned i=0;i<n;i++){if(ng_block(e,blocks[i],buf,planned)){kfree(buf);return -VFS_EIO;}size_t take=in.size-(size_t)i*fs->bs;if(take>fs->bs)take=fs->bs;memcpy((uint8_t *)bytes+(size_t)i*fs->bs,buf,take);}
 kfree(buf);*length=in.size;if(id)*id=ino;if(allocated)*allocated=total;return 0;
}
static const char *ng_intent_path(unsigned phase){return phase==PG_RETIRING?"/mnt/pkg/transaction.v1/retired.v1":ng_names[3];}
static int ng_records(ext4_engine_t *e,bool planned,size_t *pn,size_t *nn,size_t *an){
 const unsigned indexes[]={0,1,2,6,7};uint8_t *buf[]={ng_m,ng_l,ng_t,ng_s,ng_tr};const size_t sizes[]={512,1024,8192,1024,1024};size_t n;
 for(unsigned i=0;i<5;i++)if(ng_file(e,ad_paths[indexes[i]],buf[i],sizes[i],&n,0600,planned,NULL,NULL)||n!=sizes[i])return -VFS_EIO;
 unsigned phase=e4_u16(ng_l+22);*pn=*nn=*an=0;
 int r=ng_file(e,ng_intent_path(phase),ng_p,1024,pn,0600,planned,NULL,NULL);if(r&&r!=-VFS_ENOENT)return r;
 r=ng_file(e,ng_names[2],ng_n,1024,nn,0600,planned,NULL,NULL);if(r&&r!=-VFS_ENOENT)return r;
 r=ng_file(e,phase==PG_IDLE&&ad_u64(ng_s+112)?"/mnt/pkg/installed.fpk":ng_names[0],ng_art,8192,an,0600,planned,NULL,NULL);return r==-VFS_ENOENT?0:r;
}
static int ng_extra(void){for(unsigned i=436;i<440;i++)if(ad_state[i])return -VFS_EIO;for(unsigned i=592;i<992;i++)if(ad_state[i])return -VFS_EIO;return !ad_u64(ad_state+440)?-VFS_EIO:0;}
static int ng_stamp(ext4_engine_t *e){
 size_t pn,nn,an;int r=ng_records(e,true,&pn,&nn,&an);if(r)return r;
 memset(ad_state+436,0,556);ad_p64(ad_state+440,ad_u64(ng_l+24));ad_hash(ng_l,1024,ad_state+448);ad_hash(ng_tr,1024,ad_state+480);ad_hash(ng_s,1024,ad_state+512);if(pn)ad_hash(ng_p,pn,ad_state+544);memcpy(ad_state+576,ng_l+40,16);return 0;
}
static int ng_names_check(ext4_engine_t *e,const pm_view *v){
 uint32_t pkg;int pr=ad_path(e->fs,ad_paths[8],&pkg);if(pr)return pr;e4_inode_t parent;if(e4_inode(e->fs,pkg,&parent))return -VFS_EIO;
 for(uint64_t cookie=0;cookie<16;cookie++){vfs_dirent_t ent;uint32_t id;pr=e4_scan(&parent,NULL,cookie,&ent,&id);if(pr<0)return pr;if(!pr)break;bool allow=!strcmp(ent.name,".")||!strcmp(ent.name,"..");
  for(unsigned i=0;i<8;i++){const char *name=ad_paths[i]+9;allow|=!strcmp(ent.name,name);}
  allow|=v->grant.phase==PG_IDLE&&v->installed&&!strcmp(ent.name,"installed.fpk");if(!allow)return -VFS_EIO;if(cookie==15)return -VFS_EFBIG;
 }
 uint32_t ino;int r=ad_path(e->fs,ad_paths[3],&ino);if(r)return r;e4_inode_t in;if(e4_inode(e->fs,ino,&in))return -VFS_EIO;
 for(uint64_t cookie=0;cookie<16;cookie++){
  vfs_dirent_t ent;uint32_t id;r=e4_scan(&in,NULL,cookie,&ent,&id);if(r<0)return r;if(!r)return 0;
  if(!strcmp(ent.name,".")||!strcmp(ent.name,".."))continue;
  bool allow=v->grant.phase!=PG_IDLE&&(!strcmp(ent.name,"artifact.fpk")||!strcmp(ent.name,"next.v1")||!strcmp(ent.name,"activation"));
  allow|=v->grant.phase==PG_PENDING&&!strcmp(ent.name,"pending.v1");allow|=v->grant.phase==PG_RETIRING&&!strcmp(ent.name,"retired.v1");if(!allow)return -VFS_EIO;
 }
 return -VFS_EFBIG;
}
/* Remaining fixed-role capacity, including unlinked account occupancy. The
 * full account provenance validator runs in the same unpublished/exclusive
 * admission. A semantic plan changes remaining capacity by its actual F delta;
 * a rename cannot manufacture credit and an unlink alone refunds nothing. */
static int ng_account_left(ext4_engine_t *e,uint64_t left[2]){
 uint64_t d=(ACCOUNT_FILE_MAX+e->fs->bs-1)/e->fs->bs;
 left[0]=9*(d+(d>4))+2;left[1]=ACCOUNT_ROLE_COUNT;
 uint32_t seen[ACCOUNT_ROLE_COUNT+64];unsigned count=0;
 for(unsigned i=0;i<ACCOUNT_ROLE_COUNT+e->orphan_count;i++){
  uint32_t ino;bool orphan=i>=ACCOUNT_ROLE_COUNT;
  if(orphan)ino=e->orphans[i-ACCOUNT_ROLE_COUNT];
  else {int r=ad_path(e->fs,sv_path(i),&ino);if(r==-VFS_ENOENT)continue;if(r)return r;}
  for(unsigned j=0;j<count;j++)if(seen[j]==ino)return -VFS_EIO;
  if(count>=ACCOUNT_ROLE_COUNT+64)return -VFS_EFBIG;
  seen[count++]=ino;e4_inode_t in;e->fs->orphan_access=orphan?ino:0;
  int r=e4_inode(e->fs,ino,&in);e->fs->orphan_access=0;if(r)return r;
  e->fs->rw_ino=0;r=e4_mapping(&in);if(r)return r;
  uint64_t blocks=0;for(unsigned j=0;j<e->fs->physical_count;j++)blocks+=e->fs->physical[j].len;
  uint64_t max=!orphan&&i>=ACCOUNT_INTENT_TEMP?1:d+(d>4);
  if(blocks>max||blocks>left[0]||!left[1])return -VFS_EIO;
  left[0]-=blocks;left[1]--;
 }
 return 0;
}
static int ng_refine(ext4_engine_t *e,bool planned,const pm_view *v){
 size_t n;uint32_t ino;uint64_t blocks;uint8_t *b=kmalloc(8192);if(!b)return -VFS_ENOMEM;int r=0;unsigned count=e4_u16(ng_t+22),expected=v->grant.phase==PG_IDLE?0:v->grant.phase==PG_RESERVED?3:4;
 if(count!=expected){r=-VFS_EIO;goto out;}
 r=ng_names_check(e,v);if(r)goto out;
 for(unsigned i=0;i<count;i++){
  const uint8_t *entry=ng_t+256+64*i;unsigned role=e4_u16(entry);const char *path=ng_names[i];if(role!=ng_role[i]||e4_u16(entry+2)!=1||e4_u32(entry+12)!=1){r=-VFS_EIO;goto out;}
  if(role==19)path=ng_intent_path(v->grant.phase);
  if(role==17){uint32_t dest;int found=ad_path(e->fs,"/mnt/usr/bin/demo",&dest);if(!found)path="/mnt/usr/bin/demo";else if(found!=-VFS_ENOENT){r=found;goto out;}}
  r=ng_file(e,path,b,8192,&n,role==17?0755:0600,planned,&ino,&blocks);if(r)goto out;
  e4_inode_t in;if(e4_inode(e->fs,ino,&in)||ino!=e4_u32(entry+4)||in.generation!=e4_u32(entry+8)||n!=ad_u64(entry+32)||blocks!=ad_u64(entry+40)||ad_u64(entry+48)!=1){r=-VFS_EIO;goto out;}
 }
 /* Refuse unexplained fixed roles, duplicated source/destination and aliases. */
 for(unsigned i=0;i<4;i++){uint32_t id;int found=ad_path(e->fs,ng_names[i],&id);if(found&&found!=-VFS_ENOENT){r=found;goto out;}if(!found&&(v->grant.phase==PG_IDLE||(i==3&&v->grant.phase!=PG_PENDING))){r=-VFS_EIO;goto out;}}
 {uint32_t id;int found=ad_path(e->fs,"/mnt/pkg/transaction.v1/retired.v1",&id);if(found&&found!=-VFS_ENOENT){r=found;goto out;}if(!found&&v->grant.phase!=PG_RETIRING){r=-VFS_EIO;goto out;}}
 r=ng_file(e,"/mnt/usr/bin/demo",b,8192,&n,0755,planned,&ino,&blocks);
 if(!r){
  uint32_t stage;int lookup=ad_path(e->fs,ng_names[1],&stage);if(lookup!=-VFS_ENOENT||v->grant.phase==PG_RESERVED){r=-VFS_EIO;goto out;}
  const uint8_t *s=v->grant.phase==PG_IDLE?ng_s:ng_n;
  uint8_t hash[32];ad_hash(b,n,hash);if(n!=ad_u64(s+184)||memcmp(hash,s+152,32)){r=-VFS_EIO;goto out;}
 }else if(r==-VFS_ENOENT){r=0;if(v->installed||v->grant.phase==PG_RETIRING){r=-VFS_EIO;goto out;}}
 else goto out;
 if(v->grant.phase!=PG_IDLE){r=ng_file(e,ng_names[1],b,8192,&n,0755,planned,&ino,&blocks);if(!r){uint8_t h[32];ad_hash(b,n,h);if(n!=ad_u64(ng_n+184)||memcmp(h,ng_n+152,32))r=-VFS_EIO;}else if(r==-VFS_ENOENT)r=0;if(r)goto out;}
 /* Private account recovery may consume its own fixed-role capacity. Ordinary
  * allocation retains the production floor; no production allocator is changed. */
 {uint8_t *sb=kmalloc(e->fs->bs);if(!sb){r=-VFS_ENOMEM;goto out;}r=ng_block(e,1024/e->fs->bs,sb,planned);
  if(!r){uint8_t *s=sb+1024%e->fs->bs;uint64_t need[2];r=ng_account_left(e,need);
   uint8_t old[8];if(!r&&!e4_bytes(e->fs,1036,old,sizeof old))r=-VFS_EIO;
   for(unsigned i=0;!r&&i<2;i++){uint64_t free=e4_u32(s+12+4*i),before=e4_u32(old+4*i);
    if(free>=before){if(free-before>UINT64_MAX-need[i]){r=-VFS_EIO;break;}need[i]+=free-before;}
    else if(e->fs->account_allocation||e->fs->account_namespace_operation){
     if(before-free>need[i]){r=-VFS_EIO;break;}need[i]-=before-free;}
    if(free<v->grant.remaining[i]||free-v->grant.remaining[i]<need[i])r=-VFS_ENOSPC;}}
  kfree(sb);}
 out:kfree(b);return r;
}
static int ng_gate(ext4_engine_t *e,bool planned){
 if(!ng_required)return 0;
 size_t pn,nn,an;int r=ng_records(e,planned,&pn,&nn,&an);if(r)return r;
 pm_workspace *ctx=kcalloc(1,sizeof *ctx);if(!ctx)return -VFS_ENOMEM;
 pm_input in={.marker=ng_m,.ledger=ng_l,.owners=ng_t,.state=ng_s,.trust=ng_tr,.pending=ng_p,.pending_size=pn,.next=ng_n,.next_size=nn,.artifact=ng_art,.artifact_size=an,.account=ad_state,.enrollment=&ad_en,.policy=&ng_policy,.base_version=ng_base,.platform=ng_platform};pm_view view;
 memset(&view,0xa5,sizeof view);pm_view previous;memcpy(&previous,&view,sizeof view);
 int validation=pm_validate(&in,ctx,&view);if(validation)assert(!memcmp(&previous,&view,sizeof view)&&!ctx->verify.verified);
 r=validation?-VFS_EIO:ng_refine(e,planned,&view);kfree(ctx);return r;
}
static int ng_headroom(ext4_mount_t *fs){
 if(!fs||!fs->trusted_root)return -VFS_EINVAL;
 uint64_t irq=spin_lock_irqsave(&e4_lock);
 int r=fs->engine->tainted||fs->quiesced?-VFS_EIO:fs->frozen?-VFS_EROFS:ad_validate(fs->engine);
 spin_unlock_irqrestore(&e4_lock,irq);return r;
}
static void ng_seed_file(const char *path,const char *source,unsigned mode){size_t n;uint8_t *p=ng_input(source,&n);ad_write(path,p,n);free(p);creds_t root;creds_init_root(&root);assert(!vfs_chmod_creds(path,mode,&root));}
static void ng_seed_files(ext4_mount_t *fs){
 (void)fs;const uint8_t competing='X';ad_write("/mnt/pkg-competing",&competing,1);ng_seed_file(ad_paths[7],"trust.bin",0600);ng_seed_file(ad_paths[6],ng_publication==2?"next.bin":"old.bin",0600);
 if(ng_phase){ng_seed_file(ng_names[0],"artifact.fpk",0600);ng_seed_file(ng_names[1],"executable.bin",0755);ng_seed_file(ng_names[2],"next.bin",0600);if(ng_phase>=2)ng_seed_file(ng_intent_path(ng_phase),"pending.bin",0600);}
 if(ng_publication){if(ng_phase)assert(!vfs_rename(ng_names[1],"/mnt/usr/bin/demo"));else {ng_seed_file("/mnt/usr/bin/demo","executable.bin",0755);ng_seed_file("/mnt/pkg/installed.fpk","artifact.fpk",0600);}}
}
static void ng_seed_records(ext4_mount_t *fs){
 ad_hash("Fortress bounded mount profile v1",sizeof("Fortress bounded mount profile v1")-1,ad_body+152);
 memset(ng_m,0,512);memcpy(ng_m,"FORT-PKG-ENR-v1",16);e4_p32(ng_m+16,512);e4_p16(ng_m+20,1);ad_p64(ng_m+24,5);
 memcpy(ng_m+32,ad_body+40,80);memcpy(ng_m+112,ad_body+152,32);for(unsigned i=0;i<3;i++){e4_p32(ng_m+144+8*i,ad_ids[i+1]);e4_p32(ng_m+148+8*i,e4_u32(ad_body+416+128*(i+1)+8));}e4_p32(ng_m+168,2);e4_p32(ng_m+172,1);ad_hash(ng_m,480,ng_m+480);
 memset(ng_l,0,1024);memcpy(ng_l,"FORT-PKG-GRT-v1",16);e4_p32(ng_l+16,1024);e4_p16(ng_l+20,1);e4_p16(ng_l+22,ng_phase);ad_p64(ng_l+24,1);ad_p64(ng_l+32,5);if(ng_phase)memset(ng_l+40,0xa1,16);
 memcpy(ng_l+56,ad_body+104,16);memcpy(ng_l+72,ad_body+40,16);memcpy(ng_l+88,ad_body+88,16);memcpy(ng_l+104,ad_body+152,32);
 size_t n;uint8_t *old=ng_input("old.bin",&n);assert(n==1024);ad_hash(old,n,ng_l+168);free(old);uint8_t *next=ng_input("next.bin",&n);assert(n==1024);
 if(!ng_phase){uint8_t *current=ng_input(ng_publication==2?"next.bin":"old.bin",&n);ad_hash(current,n,ng_l+168);memcpy(ng_l+200,ng_l+168,32);ad_p64(ng_l+360,ad_u64(current+24));ad_p64(ng_l+368,ad_u64(current+24));free(current);}
 else{e4_p16(ng_l+348,1);ad_p64(ng_l+360,1);if(ng_phase>=2){ad_hash(next,n,ng_l+200);ad_p64(ng_l+368,2);uint8_t *p=ng_input("pending.bin",&n);assert(n==1024);ad_hash(p,n,ng_l+136);free(p);}uint8_t plan[96];ad_hash(next,1024,plan);uint8_t *a=ng_input("artifact.fpk",&n);ad_hash(a,n,plan+32);free(a);memcpy(plan+64,ad_body+152,32);ad_hash(plan,96,ng_l+392);}
 free(next);uint8_t *trust=ng_input("trust.bin",&n);assert(n==1024);ad_hash(trust,n,ng_l+232);memcpy(ad_body+248,ng_l+232,32);free(trust);
 e4_p32(ng_l+344,1);e4_p32(ng_l+352,ad_ids[3]);e4_p32(ng_l+356,e4_u32(ad_body+416+3*128+8));ad_p64(ng_l+376,3);ad_p64(ng_l+384,7);
 memset(ng_t,0,8192);memcpy(ng_t,"FORT-PKG-OWN-v1",16);e4_p32(ng_t+16,8192);e4_p16(ng_t+20,1);ad_p64(ng_t+24,5);memcpy(ng_t+32,ng_l+40,16);ad_p64(ng_t+48,1);memcpy(ng_t+56,ng_l+56,16);memcpy(ng_t+72,ng_l+72,16);memcpy(ng_t+88,ng_l+88,16);memcpy(ng_t+104,ng_l+104,32);
 if(ng_phase){unsigned count=ng_phase>=2?4:3;e4_p16(ng_t+22,count);uint64_t blocks=0;
  for(unsigned i=0;i<count;i++){uint8_t *p=ng_t+256+64*i;uint32_t ino;uint64_t allocated;size_t size;uint8_t b[8192];const char *path=i==3?ng_intent_path(ng_phase):i==1&&ng_publication?"/mnt/usr/bin/demo":ng_names[i];assert(!ng_file(fs->engine,path,b,sizeof b,&size,i==1?0755:0600,false,&ino,&allocated));e4_inode_t in;assert(!e4_inode(fs,ino,&in));e4_p16(p,ng_role[i]);e4_p16(p+2,1);e4_p32(p+4,ino);e4_p32(p+8,in.generation);e4_p32(p+12,1);memcpy(p+16,ng_l+40,16);ad_p64(p+32,size);ad_p64(p+40,allocated);ad_p64(p+48,1);blocks+=allocated;}
  ad_p64(ng_l+296,blocks+12);ad_p64(ng_l+304,count+4);ad_p64(ng_l+312,12);ad_p64(ng_l+320,4);ad_p64(ng_l+328,blocks);ad_p64(ng_l+336,count);
 }
 ad_hash(ng_t,8160,ng_t+8160);ad_hash(ng_t,8192,ng_l+264);ad_seal(ng_l);ad_write(ad_paths[0],ng_m,512);ad_write(ad_paths[1],ng_l,1024);ad_write(ad_paths[2],ng_t,8192);ad_hash(ng_m,512,ad_body+120);
 ad_hash("Fortress private account/grant v3",sizeof("Fortress private account/grant v3")-1,ad_body+312);
 uint8_t *base=ng_input("base.bin",&n);assert(n==40);ad_hash(base,n,ad_body+280);free(base);
}
/* No ambient package writer: protect mapped data and exact inode slots.
 * Adjacent account inodes may share a table block and remain admissible. */
static int ng_fence(ext4_engine_t *e){
 if(!ng_required||ng_typed)return 0;
 uint32_t protected[24];unsigned count=0;
 for(unsigned i=0;i<9;i++)if(i!=4&&i!=5){uint32_t id;if(ad_path(e->fs,ad_paths[i],&id))return -VFS_EIO;protected[count++]=id;}
 const char *extra[]={ng_names[0],ng_names[1],ng_names[2],ng_names[3],"/mnt/pkg/transaction.v1/retired.v1","/mnt/pkg/installed.fpk","/mnt/usr/bin/demo"};
 for(unsigned i=0;i<7;i++){uint32_t id;int r=ad_path(e->fs,extra[i],&id);if(!r)protected[count++]=id;else if(r!=-VFS_ENOENT)return r;}
 for(unsigned i=0;i<count;i++){
  e4_inode_t in;if(e4_inode(e->fs,protected[i],&in))return -VFS_EIO;e->fs->rw_ino=0;if(e4_mapping(&in))return -VFS_EIO;
  for(unsigned j=0;j<e->images;j++)for(unsigned k=0;k<e->fs->physical_count;k++)if(e->image[j].block>=e->fs->physical[k].lo&&(uint64_t)e->image[j].block<e->fs->physical[k].lo+e->fs->physical[k].len)return -VFS_EROFS;
  uint32_t g=(protected[i]-1)/e->fs->ipg,b=(protected[i]-1)%e->fs->ipg;uint64_t at=(uint64_t)e4_u32(e->fs->gd[g].raw+8)*e->fs->bs+b*256;uint8_t raw[256];
  if(!e4_bytes(e->fs,at,raw,256))return -VFS_EIO;
  for(unsigned j=0;j<e->images;j++)if(e->image[j].block==at/e->fs->bs&&memcmp(e->image[j].bytes+at%e->fs->bs,raw,256))return -VFS_EROFS;
 }
 return 0;
}
static void ng_guard_tests(ext4_mount_t *fs){
 const char *paths[]={"/pkg/grant.v1","/pkg/state.v1","/pkg/trust.v1"};
 for(unsigned i=0;i<3;i++){int error;vfs_node_t *n=e4_ac_lookup(fs,paths[i],&error);assert(n);uint64_t off=0;uint8_t b=0;size_t events=ad_disk->events;
  assert(n->write(n,&off,false,&b,1)==-VFS_EROFS&&ad_disk->events==events&&!fs->engine->tainted);vfs_node_put(n);}
 uint64_t irq=spin_lock_irqsave(&e4_lock),left[2];uint8_t free[8];
 assert(!ng_account_left(fs->engine,left)&&e4_bytes(fs,1036,free,sizeof free));
 bool exact=true;for(unsigned i=0;i<2;i++)exact&=e4_u32(free+4*i)-ad_u64(ng_l+312+8*i)==left[i];
 if(exact){size_t events=ad_disk->events;uint32_t ino=0;vfs_create_attrs_t attrs={.mode=0600};
  assert(ext4_engine_namespace_create_attrs(fs->engine,2,"pkg-competing-new",false,&attrs,&ino)==-VFS_ENOSPC);
  assert(ad_disk->events==events&&!fs->engine->tainted);}
 spin_unlock_irqrestore(&e4_lock,irq);
 if(exact){int error;vfs_node_t *n=e4_ac_lookup(fs,"/pkg-competing",&error);assert(n);uint64_t off=fs->bs;uint8_t b='Y';size_t events=ad_disk->events;
  assert(n->write(n,&off,false,&b,1)==-VFS_ENOSPC&&ad_disk->events==events&&!fs->engine->tainted);vfs_node_put(n);
  printf("PASS competing block/inode zero-io\n");}
}
long tool_syscall(long nr,uintptr_t a,uintptr_t b,uintptr_t c){(void)nr;(void)a;(void)b;(void)c;assert(!"unexpected package verifier syscall");return -1;}
static void ng_preview(ext4_engine_t *e){
 size_t pn,nn,an;assert(!ng_records(e,false,&pn,&nn,&an));uint64_t at=ad_u64(ad_locator(1)+48)*e->fs->bs;
 bool newer=ad_u64(ad_disk->stable+at+24)!=ad_u64(ng_l+24);ng_newer+=newer;
 printf("PM preview phase=%u grant=%llu newer=%u G=%llu/%llu Q=%llu/%llu\n",e4_u16(ng_l+22),(unsigned long long)ad_u64(ng_l+24),newer,(unsigned long long)ad_u64(ng_l+312),(unsigned long long)ad_u64(ng_l+320),(unsigned long long)ad_u64(ng_l+328),(unsigned long long)ad_u64(ng_l+336));
}
static int ng_stage(ext4_engine_t *e,unsigned index,const uint8_t *bytes,size_t n){
 uint32_t ino;if(ad_path(e->fs,ad_paths[index],&ino))return -VFS_EIO;e4_inode_t in;if(e4_inode(e->fs,ino,&in)||in.size!=n)return -VFS_EIO;
 e->fs->rw_ino=0;if(e4_mapping(&in))return -VFS_EIO;uint32_t blocks[8];unsigned count=(unsigned)((n+e->fs->bs-1)/e->fs->bs);if(count>8)return -VFS_EIO;
 for(unsigned i=0;i<count;i++){const e4_extent_t *x=e4_find(&in,i);if(!x)return -VFS_EIO;blocks[i]=x->physical+i-x->logical;}
 for(unsigned i=0;i<count;i++){uint8_t *p=e4_stage(e,blocks[i],E4_REFERENCE);if(!p)return e->error;size_t take=n-(size_t)i*e->fs->bs;if(take>e->fs->bs)take=e->fs->bs;memcpy(p,bytes+(size_t)i*e->fs->bs,take);}return 0;
}
static int ng_tick(ext4_mount_t *fs){
 uint64_t irq=spin_lock_irqsave(&e4_lock);pe_origin o;int r=ad_pair(fs->engine,false,&o);
 if(!r&&(o.active||e4_u16(ad_state+22)||ad_u64(ng_l+24)>=UINT64_MAX-1))r=-VFS_EAGAIN;
 if(!r)r=e4_namespace_begin(fs->engine);
 if(!r){ng_typed=true;ad_p64(ng_l+24,ad_u64(ng_l+24)+1);ad_p64(ng_t+48,ad_u64(ng_l+24));ad_hash(ng_t,8160,ng_t+8160);ad_hash(ng_t,8192,ng_l+264);ad_seal(ng_l);
  r=ng_stage(fs->engine,1,ng_l,1024);if(!r)r=ng_stage(fs->engine,2,ng_t,8192);if(!r)r=ad_store(fs->engine);r=e4_namespace_end(fs->engine,r);ng_typed=false;}
 spin_unlock_irqrestore(&e4_lock,irq);return r;
}
int main(int argc,char **argv){
 ng_config();
 if(argc!=7||strncmp(argv[1],"tick",4))return ng_service_main(argc,argv);
 setvbuf(stdout,NULL,_IONBF,0);size_t size;uint8_t *initial=load(argv[2],&size);unsigned ss=(unsigned)strtoul(argv[3],NULL,10);
 ext4_fault_disk_t d={.stable=malloc(size),.volatile_bytes=malloc(size),.bytes=size,.cut=-1};assert(d.stable&&d.volatile_bytes);ad_disk=&d;
 block_dev_t dev={.sector_size=ss,.sector_count=size/ss,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};reset(&d,initial);ad_auth(argv[4],argv[5]);sv_mount(&dev);assert(!accounts_prepare(sv_fs));
 ng_pause=!strcmp(argv[1],"tick-pause");bool pause=ng_pause;d.events=d.writes=d.flushes=0;
 const char *eq=NULL;for(const char *p=argv[1];*p;p++)if(*p=='=')eq=p;
 if(eq){unsigned profile;long cut;assert(sscanf(eq+1,"%u:%ld",&profile,&cut)==2&&profile<8&&cut>=0);d.persistence=profile/2;d.after=profile%2;d.cut=cut;}
 int r=ng_tick(sv_fs);if(pause||eq){assert(r==-VFS_EIO);if(eq)assert(d.offline);accounts_abandon();}else assert(!r);
 printf("PASS tick events=%zu pending=%u\n",d.events,pause);save(argv[6],"interrupted",d.stable,size);ad_enabled=false;teardown();free(initial);free(d.stable);free(d.volatile_bytes);return 0;
}
