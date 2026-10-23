/* Private actual EXT4/JBD2 + account-service workbench. No production caller. */
#include "pkg_retirement_auth.h"
static const char *xr_paths[]={"/mnt/pkg/retirement.v1","/mnt/pkg/retirement.cert","/mnt/pkg/enrollment.cert","/mnt/pkg/evidence.fpk"};
static uint8_t xr_locations[512];
static pe_policy xr_en_policy;
static pa_policy xr_policy;
static bool xr_bootstrap;
static unsigned xr_previews,xr_commits,xr_max_images;
typedef struct {
 uint8_t m[512],g[1024],t[8192],l[1024],a[1024],o[1024],s[1024],tr[1024],intent[1024];
 uint8_t en[PE_MAX],cert[PA_MAX],art[8192],exe[8192];
 pa_input input;
} xr_bundle;
static xr_bundle xr_old,xr_new;
static pa_workspace xr_workspace;
static pa_result xr_result;
static void xr_seed(ext4_mount_t *fs){
 (void)fs;static uint8_t zero[PE_MAX];
 ad_write(xr_paths[0],zero,1024);ad_write(xr_paths[1],zero,PA_BODY+192);ad_write(xr_paths[2],zero,PE_BODY+192);
 ng_seed_file(xr_paths[3],"artifact.fpk",0600);
}
static void xr_policy_load(const uint8_t *p,size_t n){
 assert(n==1568);memset(&xr_en_policy,0,sizeof xr_en_policy);
 memcpy(xr_en_policy.keys,p,256);memcpy(xr_en_policy.uuid,p+256,80);memcpy(xr_en_policy.descriptor_sha256,p+336,32);
 xr_en_policy.key_count=e4_u32(p+368);xr_en_policy.threshold=e4_u32(p+372);xr_en_policy.epoch=ad_u64(p+376);
 memcpy(xr_policy.keys,p+760,256);xr_policy.key_count=e4_u32(p+1016);xr_policy.threshold=e4_u32(p+1020);
 memcpy(xr_policy.certificate_sha256,p+1024,32);memcpy(xr_locations,p+1056,512);
 xr_policy.enrollment=&xr_en_policy;xr_policy.catalog=&ng_policy;xr_policy.platform=ng_platform;xr_policy.base_version=ng_base;
 xr_active=true;
}
static int xr_file(ext4_engine_t *e,const char *path,uint8_t *bytes,size_t cap,bool planned,size_t *size){
 return ng_file(e,path,bytes,cap,size,0600,planned,NULL,NULL);
}
static int xr_load(ext4_engine_t *e,bool planned,xr_bundle *b){
 size_t n;
 const char *paths[]={ad_paths[0],ad_paths[1],ad_paths[2],xr_paths[0],ad_paths[5],ad_paths[4],ad_paths[6],ad_paths[7],"/mnt/pkg/transaction.v1/retired.v1"};
 uint8_t *buf[]={b->m,b->g,b->t,b->l,b->a,b->o,b->s,b->tr,b->intent};
 const size_t sizes[]={512,1024,8192,1024,1024,1024,1024,1024,1024};
 for(unsigned i=0;i<9;i++)if(xr_file(e,paths[i],buf[i],sizes[i],planned,&n)||n!=sizes[i])return -VFS_EIO;
 size_t en,cert,art,exe;
 if(xr_file(e,xr_paths[2],b->en,sizeof b->en,planned,&en)||
    xr_file(e,xr_paths[1],b->cert,sizeof b->cert,planned,&cert)||
    xr_file(e,xr_paths[3],b->art,sizeof b->art,planned,&art)||
    ng_file(e,"/mnt/usr/bin/demo",b->exe,sizeof b->exe,&exe,0755,planned,NULL,NULL))return -VFS_EIO;
 b->input=(pa_input){.records={.marker=b->m,.marker_n=512,.grant=b->g,.grant_n=1024,.owners=b->t,.owners_n=8192,
  .ledger=b->l,.ledger_n=1024,.account=b->a,.account_n=1024,.origin=b->o,.origin_n=1024},
  .certificate=b->cert,.certificate_n=cert,.enrollment=b->en,.enrollment_n=en,.state=b->s,.trust=b->tr,.intent=b->intent,
  .artifact=b->art,.artifact_n=art,.executable=b->exe,.executable_n=exe};
 return 0;
}
static int xr_fixed(ext4_engine_t *e){
 for(unsigned i=0;i<4;i++){
  const uint8_t *r=xr_locations+128*i;uint32_t ino;e4_inode_t in;size_t n;static uint8_t bytes[8192];uint64_t blocks;
  if(e4_u16(r)!=i+1||e4_u16(r+2)!=1||e4_u32(r+12)!=0600||ad_path(e->fs,xr_paths[i],&ino)||
     ino!=e4_u32(r+4)||e4_inode(e->fs,ino,&in)||in.generation!=e4_u32(r+8)||
     xr_file(e,xr_paths[i],bytes,sizeof bytes,false,&n)||n!=ad_u64(r+16))return -VFS_EIO;
  e->fs->rw_ino=0;if(e4_mapping(&in)||in.map->count!=e4_u16(r+32)||e->fs->physical_count!=in.map->count)return -VFS_EIO;
  blocks=0;for(unsigned j=0;j<in.map->count;j++){
   e4_extent_t x=in.map->entries[j];const uint8_t *q=r+40+16*j;
   if(x.logical!=e4_u32(q)||x.len!=e4_u32(q+4)||x.physical!=ad_u64(q+8)||x.unwritten)return -VFS_EIO;
   blocks+=x.len;
  }
  if(blocks!=ad_u64(r+24))return -VFS_EIO;
 }
 return 0;
}
static int xr_capacity(ext4_engine_t *e,uint64_t left[2]){
 int r=ng_account_left(e,left);if(r)return r;
 for(unsigned i=0;i<e->orphan_count;i++){
  uint32_t ino=e->orphans[i];if(ino!=xr_result.binding.inode)return -VFS_EIO;
  uint32_t g=(ino-1)/e->fs->ipg,bit=(ino-1)%e->fs->ipg;uint8_t raw[256];
  if(!e4_bytes(e->fs,(uint64_t)e4_u32(e->fs->gd[g].raw+8)*e->fs->bs+bit*256,raw,256)||
     e4_u32(raw+100)!=xr_result.binding.generation||e4_u32(raw+28)%(e->fs->bs/512))return -VFS_EIO;
  left[0]+=e4_u32(raw+28)/(e->fs->bs/512);left[1]++;
 }
 return 0;
}
static int xr_owned(ext4_engine_t *e,const xr_bundle *b){
 ext4_mount_t *fs=e->fs;uint64_t q[2]={0,0};unsigned orphans=0;
 const char *names[]={"/mnt/pkg/transaction.v1/artifact.fpk","/mnt/usr/bin/demo","/mnt/pkg/transaction.v1/next.v1","/mnt/pkg/transaction.v1/retired.v1"};
 if(e4_u16(b->t+22)!=4)return -VFS_EIO;
 const unsigned roles[]={1,17,18,19};
 for(unsigned i=0;i<4;i++){
  const uint8_t *row=b->t+256+64*i;uint32_t ino=e4_u32(row+4),gen=e4_u32(row+8),named=0;
  unsigned cls=e4_u16(row+2),flags=e4_u32(row+12);if(e4_u16(row)!=roles[i])return -VFS_EIO;int found=ad_path(fs,names[i],&named);
  if(found&&found!=-VFS_ENOENT)return found;
  if(ino<11||ino>fs->inodes)return -VFS_EIO;
  uint32_t group=(ino-1)/fs->ipg,bit=(ino-1)%fs->ipg;uint8_t raw[256];
  if(!e4_bitmap(fs,group,true))return -VFS_EIO;
  bool allocated=e4_bit(fs->scratch,bit);
  if(!e4_bytes(fs,(uint64_t)e4_u32(fs->gd[group].raw+8)*fs->bs+bit*256,raw,256)||e4_u32(raw+100)!=gen)return -VFS_EIO;
  if(cls==3){if(i||allocated||!found)return -VFS_EIO;continue;}
  bool orphan=flags==2;
  if(cls!=1||!allocated||e4_u16(raw+26)!=(orphan?0:1)||e4_u16(raw)!=(VFS_S_IFREG|(i==1?0755:0600))||
     e4_u16(raw+2)||e4_u16(raw+24)||e4_u16(raw+120)||e4_u16(raw+122)||
     (orphan?(!i&&found==-VFS_ENOENT&&e4_orphan_index(e,ino)>=0)==false:(found||named!=ino)))return -VFS_EIO;
  orphans+=orphan;e4_inode_t in;fs->orphan_access=orphan?ino:0;int r=e4_inode(fs,ino,&in);fs->orphan_access=0;
  if(r||in.size!=ad_u64(row+32))return -VFS_EIO;
  fs->rw_ino=0;if(e4_mapping(&in))return -VFS_EIO;uint64_t blocks=0;
  for(unsigned j=0;j<fs->physical_count;j++)blocks+=fs->physical[j].len;
  if(blocks!=ad_u64(row+40)||ad_u64(row+48)!=1||blocks*(fs->bs/512)!=e4_u32(raw+28))return -VFS_EIO;
  if(i==0){
   if(blocks>1||in.map->count>1)return -VFS_EIO;
   if(blocks){static uint8_t native[8192];if(in.size>fs->bs||in.map->count!=1||in.map->entries[0].logical||in.map->entries[0].len!=1||
     !e4_bytes(fs,(uint64_t)in.map->entries[0].physical*fs->bs,native,in.size)||in.size!=b->input.artifact_n||memcmp(native,b->art,in.size))return -VFS_EIO;}
  }
  if(i==2){static uint8_t native[1024];size_t n;if(xr_file(e,names[i],native,1024,false,&n)||n!=1024||memcmp(native,b->s,1024))return -VFS_EIO;}
  q[0]+=blocks;q[1]++;
 }
 pm_view names_view={.grant={.phase=PG_RETIRING},.installed=1};if(ng_names_check(e,&names_view))return -VFS_EIO;
 if(orphans!=e->orphan_count||q[0]!=ad_u64(b->g+328)||q[1]!=ad_u64(b->g+336))return -VFS_EIO;
 uint32_t unexpected;int r=ad_path(fs,"/mnt/pkg/transaction.v1/pending.v1",&unexpected);
 if(r!=-VFS_ENOENT)return -VFS_EIO;
 r=ad_path(fs,"/mnt/pkg/transaction.v1/activation",&unexpected);if(r!=-VFS_ENOENT)return -VFS_EIO;
 uint64_t left[2];uint8_t free[8];
 if(xr_capacity(e,left)||!e4_bytes(fs,1036,free,8))return -VFS_EIO;
 for(unsigned i=0;i<2;i++)if(e4_u32(free+4*i)<ad_u64(b->g+312+8*i)||e4_u32(free+4*i)-ad_u64(b->g+312+8*i)<left[i])return -VFS_ENOSPC;
 return 0;
}
static int xr_gate(ext4_engine_t *e,bool planned){
 int r=xr_fixed(e);if(r){fprintf(stderr,"XR fixed %d planned=%u\n",r,planned);return -VFS_EIO;}
 r=xr_load(e,planned,&xr_new);if(r){fprintf(stderr,"XR load %d planned=%u\n",r,planned);return -VFS_EIO;}
 r=pa_verify(&xr_new.input,&xr_policy,&xr_workspace,&xr_result);if(r){fprintf(stderr,"XR authority %d planned=%u\n",r,planned);return -VFS_EIO;}
 if(!planned&&xr_owned(e,&xr_new))return -VFS_EIO;
 if(planned){
  uint64_t left[2];static uint8_t block[4096];if(xr_capacity(e,left)||ng_block(e,1024/e->fs->bs,block,true))return -VFS_EIO;
  const uint8_t *super=block+1024%e->fs->bs;
  for(unsigned i=0;i<2;i++){
   uint64_t free=e4_u32(super+12+4*i),remaining=ad_u64(xr_new.g+312+8*i),capacity=i?e->fs->inodes:e->fs->blocks;
   if(free>capacity)return -VFS_EIO;
   if(free<remaining||free-remaining<left[i])return -VFS_ENOSPC;
  }
 }
 return 0;
}
static int xr_reopened(ext4_engine_t *e){
 if(!xr_active)return 0;
 int r=ad_validate(e);if(!r)xr_previews++;return r;
}
static int xr_fence(ext4_engine_t *e){
 if(xr_typed)return 0;
 uint32_t ids[32];unsigned count=0;
 for(unsigned i=0;i<9;i++)ids[count++]=e4_u32(ad_locator(i)+4);
 ids[count++]=e4_u32(ad_locator(14)+4);
 for(unsigned i=0;i<4;i++)ids[count++]=e4_u32(xr_locations+128*i+4);
 for(unsigned i=0;i<e4_u16(xr_new.t+22);i++)if(e4_u16(xr_new.t+256+64*i+2)!=3)ids[count++]=e4_u32(xr_new.t+256+64*i+4);
 for(unsigned i=0;i<count;i++){
  e4_inode_t in;bool orphan=e4_orphan_index(e,ids[i])>=0;e->fs->orphan_access=orphan?ids[i]:0;int r=e4_inode(e->fs,ids[i],&in);e->fs->orphan_access=0;if(r)return r;
  e->fs->rw_ino=0;if(e4_mapping(&in))return -VFS_EIO;
  for(unsigned j=0;j<e->images;j++)for(unsigned k=0;k<e->fs->physical_count;k++)if(e->image[j].block>=e->fs->physical[k].lo&&(uint64_t)e->image[j].block<e->fs->physical[k].lo+e->fs->physical[k].len)return -VFS_EROFS;
  uint32_t g=(ids[i]-1)/e->fs->ipg,bit=(ids[i]-1)%e->fs->ipg;uint64_t at=(uint64_t)e4_u32(e->fs->gd[g].raw+8)*e->fs->bs+bit*256;uint8_t raw[256];
  if(!e4_bytes(e->fs,at,raw,256))return -VFS_EIO;
  for(unsigned j=0;j<e->images;j++)if(e->image[j].block==at/e->fs->bs&&memcmp(raw,e->image[j].bytes+at%e->fs->bs,256))return -VFS_EROFS;
 }
 return 0;
}
static int xr_stage(ext4_engine_t *e,const char *path,const uint8_t *bytes,size_t n){
 uint32_t ino;if(ad_path(e->fs,path,&ino))return -VFS_EIO;e4_inode_t in;if(e4_inode(e->fs,ino,&in)||in.size!=n)return -VFS_EIO;
 e->fs->rw_ino=0;if(e4_mapping(&in))return -VFS_EIO;uint32_t blocks[8];unsigned count=(n+e->fs->bs-1)/e->fs->bs;if(count>8)return -VFS_EIO;
 for(unsigned i=0;i<count;i++){const e4_extent_t *x=e4_find(&in,i);if(!x||x->unwritten)return -VFS_EIO;blocks[i]=x->physical+i-x->logical;}
 for(unsigned i=0;i<count;i++){uint8_t *p=e4_stage(e,blocks[i],E4_REFERENCE);if(!p)return e->error;size_t take=n-(size_t)i*e->fs->bs;if(take>e->fs->bs)take=e->fs->bs;memcpy(p,bytes+(size_t)i*e->fs->bs,take);}
 return 0;
}
static void xr_build(xr_bundle *b){
 ad_hash(b->t,8160,b->t+8160);ad_hash(b->t,8192,b->g+264);ad_seal(b->g);
 ad_p64(b->l+24,ad_u64(b->g+24));ad_hash(b->g,1024,b->l+136);ad_hash(b->t,8192,b->l+168);ad_p64(b->l+200,ad_u64(b->a+24));
 e4_p16(b->l+218,e4_u16(b->t+258));e4_p32(b->l+220,e4_u32(b->t+268));ad_p64(b->l+232,ad_u64(b->t+288));
 memcpy(b->l+256,b->g+312,32);memcpy(b->l+288,b->t+296,16);ad_seal(b->l);
 ad_p64(b->o+24,ad_u64(b->a+24));ad_seal(b->o);ad_hash(b->o,1024,b->a+112);
 ad_p64(b->a+440,ad_u64(b->g+24));ad_hash(b->g,1024,b->a+448);ad_hash(b->l,1024,b->a+592);ad_seal(b->a);
}
static int e4_semantic_commit(struct ext4_engine *e){
 if(!xr_active||!xr_typed)return xr_legacy_commit(e);
 if(xr_bootstrap){int r=xr_gate(e,true);if(!r)r=ad_core_commit(e);return r;}
 int pre=xr_load(e,false,&xr_old);if(!pre)pre=pa_verify(&xr_old.input,&xr_policy,&xr_workspace,&xr_result);
 if(pre){fprintf(stderr,"XR before auth %d\n",pre);return -VFS_EIO;}
 pre=xr_owned(e,&xr_old);if(pre){fprintf(stderr,"XR before ownership %d orphans=%u\n",pre,e->orphan_count);return -VFS_EIO;}
 uint32_t ino=e4_u32(xr_old.t+260),g=(ino-1)/e->fs->ipg,bit=(ino-1)%e->fs->ipg;
 uint8_t *map=e4_stage_bitmap(e,g,true),*raw=e4_inode_image(e,ino);if(!map||!raw)return -VFS_EIO;
 bool allocated=e4_bit(map,bit);uint64_t qb=allocated?e4_u32(raw+28)/(e->fs->bs/512):0,qi=allocated?1:0;
 if(e4_u32(raw+28)%(e->fs->bs/512)||e4_u32(raw+100)!=e4_u32(xr_old.t+264)||
    (allocated&&(e4_u16(raw)!=(VFS_S_IFREG|0600)||e4_u16(raw+26)>1)))return -VFS_EIO;
 memcpy(&xr_new,&xr_old,sizeof xr_new);
 uint64_t seq=ad_u64(xr_old.g+24),as=ad_u64(xr_old.a+24);if(seq>=UINT64_MAX-1||as>=UINT64_MAX-1)return -VFS_EFBIG;
 ad_p64(xr_new.g+24,seq+1);ad_p64(xr_new.t+48,seq+1);ad_p64(xr_new.a+24,as+1);
 uint64_t owned[2]={qb,qi};uint8_t sb[1024];if(!e4_bytes(e->fs,1024,sb,1024))return -VFS_EIO;
 uint8_t *super=e4_super(e);if(!super)return -VFS_EIO;uint64_t f[2]={e4_u32(sb+12),e4_u32(sb+16)},next[2]={e4_u32(super+12),e4_u32(super+16)};
 for(unsigned i=0;i<2;i++){
  uint64_t old=ad_u64(xr_old.t+296+8*i);if(owned[i]>old)return -VFS_EIO;uint64_t refund=old-owned[i];
  ad_p64(xr_new.g+312+8*i,ad_u64(xr_old.g+312+8*i)+refund);ad_p64(xr_new.g+328+8*i,ad_u64(xr_old.g+328+8*i)-refund);
 }
 e4_p16(xr_new.t+258,allocated?1:3);e4_p32(xr_new.t+268,allocated?(e4_u16(raw+26)?1:2):0);
 ad_p64(xr_new.t+288,allocated?ad_u64(xr_old.t+288):0);ad_p64(xr_new.t+296,qb);ad_p64(xr_new.t+304,qi);xr_build(&xr_new);
 /* Copies contain self pointers; rebuild the record view, never borrow old. */
 pr_set before=xr_old.input.records,after={.marker=xr_new.m,.marker_n=512,.grant=xr_new.g,.grant_n=1024,.owners=xr_new.t,.owners_n=8192,
  .ledger=xr_new.l,.ledger_n=1024,.account=xr_new.a,.account_n=1024,.origin=xr_new.o,.origin_n=1024};
 pr_change change;if(pr_transition(&before,&after,&xr_result.binding,f,next,&change)){
 fprintf(stderr,"XR transition F=%llu/%llu->%llu/%llu Q=%llu/%llu\n",(unsigned long long)f[0],(unsigned long long)f[1],(unsigned long long)next[0],(unsigned long long)next[1],(unsigned long long)qb,(unsigned long long)qi);return -VFS_EIO;}
 const char *paths[]={ad_paths[1],ad_paths[2],xr_paths[0],ad_paths[5],ad_paths[4]};
 const uint8_t *bytes[]={xr_new.g,xr_new.t,xr_new.l,xr_new.a,xr_new.o};size_t sizes[]={1024,8192,1024,1024,1024};
 for(unsigned i=0;i<5;i++)if(xr_stage(e,paths[i],bytes[i],sizes[i]))return -VFS_EIO;
 if(xr_gate(e,true))return -VFS_EIO;
 if(e->images>xr_max_images)xr_max_images=e->images;
 int r=ad_core_commit(e);if(!r)xr_commits++;return r;
}
static void xr_export(ext4_mount_t *fs,const char *prefix){
 memset(xr_locations,0,sizeof xr_locations);
 for(unsigned i=0;i<4;i++){
  uint32_t ino;assert(!ad_path(fs,xr_paths[i],&ino));e4_inode_t in;assert(!e4_inode(fs,ino,&in));
  fs->rw_ino=0;assert(!e4_mapping(&in)&&in.map->count<=4);uint8_t *r=xr_locations+128*i;
  e4_p16(r,i+1);e4_p16(r+2,1);e4_p32(r+4,ino);e4_p32(r+8,in.generation);e4_p32(r+12,0600);ad_p64(r+16,in.size);
  uint64_t total=0;e4_p16(r+32,in.map->count);
  for(unsigned j=0;j<in.map->count;j++){e4_extent_t x=in.map->entries[j];e4_p32(r+40+16*j,x.logical);e4_p32(r+44+16*j,x.len);ad_p64(r+48+16*j,x.physical);total+=x.len;}
  ad_p64(r+24,total);
 }
 save(prefix,"locations",xr_locations,sizeof xr_locations);
}
static int xr_account_check(void){
 int r=accounts_prepare(sv_fs);
 if(r){
  assert(r==-VFS_EIO&&!enabled&&!writer);size_t events=ad_disk->events;creds_t root;creds_init_root(&root);
  account_request_t check={.size=64,.version=1,.generation=generation};
  assert(accounts_control(ACCOUNT_CHECK,(uintptr_t)&check,sizeof check,&root,77)!=0);
  check.generation=0;assert(accounts_control(ACCOUNT_BEGIN,(uintptr_t)&check,sizeof check,&root,77)!=0&&ad_disk->events==events);
  return r;
 }
 assert(enabled&&!writer&&generation==ad_u64(ad_state+144));
 creds_t root;creds_init_root(&root);account_request_t check={.size=64,.version=1,.generation=generation};
 assert(!accounts_control(ACCOUNT_CHECK,(uintptr_t)&check,sizeof check,&root,77));
 static char files[3][8192];account_request_t snapshot={.size=64,.version=1,.flags=ACCOUNT_WITH_SHADOW,
  .passwd=(uintptr_t)files[0],.group=(uintptr_t)files[1],.shadow=(uintptr_t)files[2],
  .passwd_bytes=8192,.group_bytes=8192,.shadow_bytes=8192};
 assert(!accounts_control(ACCOUNT_SNAPSHOT,(uintptr_t)&snapshot,sizeof snapshot,&root,77)&&snapshot.generation==generation);
 assert(!sv_family());size_t events=ad_disk->events;uint64_t seq=ad_u64(ad_state+24);account_request_t begin={.size=64,.version=1};
 assert(accounts_control(ACCOUNT_BEGIN,(uintptr_t)&begin,sizeof begin,&root,77)==SYSCALL_EAGAIN&&
  ad_disk->events==events&&!writer&&enabled&&ad_u64(ad_state+24)==seq);
 puts("PASS account recovery/snapshot/check; BEGIN zero-io refused");return 0;
}
static void xr_guards(void){
 const char *names[]={"/pkg/grant.v1","/pkg/ownership.v1","/pkg/retirement.v1","/pkg/account-generation.v1","/pkg/origin.v1",
  "/pkg/retirement.cert","/pkg/enrollment.cert","/pkg/evidence.fpk","/pkg/state.v1","/pkg/trust.v1","/pkg/transaction.v1/retired.v1","/usr/bin/demo"};
 for(unsigned i=0;i<sizeof(names)/sizeof(*names);i++){
  int err;vfs_node_t *n=e4_ac_lookup(sv_fs,names[i],&err);assert(n);uint64_t off=0;uint8_t byte=0;size_t events=ad_disk->events;
  assert(n->write(n,&off,false,&byte,1)<0&&events==ad_disk->events&&!sv_fs->engine->tainted);vfs_node_put(n);
 }
 uint64_t irq=spin_lock_irqsave(&e4_lock);assert(!ad_validate(sv_fs->engine));spin_unlock_irqrestore(&e4_lock,irq);
 int error;vfs_node_t *victim=e4_ac_lookup(sv_fs,"/pkg/transaction.v1/artifact.fpk",&error);
 if(victim){size_t events=ad_disk->events;assert(victim->parent->unlink(victim->parent,"artifact.fpk")<0&&ad_disk->events==events&&!sv_fs->engine->tainted);vfs_node_put(victim);}
 uint64_t irq2=spin_lock_irqsave(&e4_lock),left[2];uint8_t free[8];assert(!xr_capacity(sv_fs->engine,left)&&e4_bytes(sv_fs,1036,free,8));
 bool exact=true;for(unsigned i=0;i<2;i++)exact&=e4_u32(free+4*i)-ad_u64(xr_new.g+312+8*i)==left[i];
 if(exact){size_t events=ad_disk->events;uint32_t ino=0;vfs_create_attrs_t attrs={.mode=0600};
  int r=ext4_engine_namespace_create_attrs(sv_fs->engine,2,"retirement-competing",false,&attrs,&ino);
  if(r!=-VFS_ENOSPC)fprintf(stderr,"XR competing inode result=%d events=%zu\n",r,ad_disk->events-events);
  assert(r==-VFS_ENOSPC&&ad_disk->events==events&&!sv_fs->engine->tainted);
 }
 spin_unlock_irqrestore(&e4_lock,irq2);
 if(exact){int error;vfs_node_t *n=e4_ac_lookup(sv_fs,"/pkg-competing",&error);assert(n);uint64_t off=sv_fs->bs;uint8_t b='Y';size_t events=ad_disk->events;
  int64_t r=n->write(n,&off,false,&b,1);if(r!=-VFS_ENOSPC)fprintf(stderr,"XR competing block result=%lld events=%zu\n",(long long)r,ad_disk->events-events);
  assert(r==-VFS_ENOSPC&&ad_disk->events==events&&!sv_fs->engine->tainted);vfs_node_put(n);
  puts("PASS exact reserve competing block/inode allocations zero-io refused");
 }
 puts("PASS protected writes and untyped retirement zero-io refused");
}
static void xr_stats(void){
 uint64_t irq=spin_lock_irqsave(&e4_lock);assert(!ad_validate(sv_fs->engine));spin_unlock_irqrestore(&e4_lock,irq);
 uint64_t capacity[2];assert(!xr_capacity(sv_fs->engine,capacity));
 unsigned d=(8192+sv_fs->bs-1)/sv_fs->bs;assert(capacity[0]==9*(d+(d>4))+2-3&&capacity[1]==8);
 printf("XR reserve=%llu/%llu\n",(unsigned long long)capacity[0],(unsigned long long)capacity[1]);
 printf("XR seq=%llu account=%llu class=%u flags=%u G=%llu/%llu Q=%llu/%llu commits=%u images=%u previews=%u\n",
  (unsigned long long)ad_u64(xr_new.g+24),(unsigned long long)ad_u64(xr_new.a+24),e4_u16(xr_new.t+258),e4_u32(xr_new.t+268),
  (unsigned long long)ad_u64(xr_new.g+312),(unsigned long long)ad_u64(xr_new.g+320),
  (unsigned long long)ad_u64(xr_new.g+328),(unsigned long long)ad_u64(xr_new.g+336),xr_commits,xr_max_images,xr_previews);
}
static const char *xr_equals(const char *p){while(*p){if(*p=='=')return p;p++;}return NULL;}
static void xr_arm(ext4_fault_disk_t *d,const char *mode){
 d->events=d->writes=d->flushes=0;
 const char *eq=xr_equals(mode);if(eq){unsigned profile;long cut;assert(sscanf(eq+1,"%u:%ld",&profile,&cut)==2&&profile<8&&cut>=0);d->persistence=profile/2;d->after=profile%2;d->cut=cut;}
 ng_pause=strstr(mode,"pause")!=NULL;
}
/* Refusal during first activation is allowed only while the expected complete
 * certificate is not visible. Inspect a read-only replay view so a committed
 * mixed generation cannot be mislabeled as an unpublished enrollment. */
static bool xr_certificate_visible(block_dev_t *dev){
 const uint8_t *r=xr_locations+128;uint64_t n=ad_u64(r+16),bs=e4_u32(ad_en.wire+36),physical=ad_u64(r+48);
 if(!n||n>PA_MAX||e4_u16(r+32)!=1||!bs||physical>UINT64_MAX/bs||!dev->sector_size||
    dev->sector_count>UINT64_MAX/dev->sector_size)return false;
 uint64_t offset_limit=physical*bs,device_bytes=dev->sector_count*dev->sector_size;
 if(offset_limit>device_bytes||n>device_bytes-offset_limit)return false;
 static uint8_t bytes[PA_MAX],sector[4096];uint8_t hash[32];jbd2_plan_t *plan=NULL;
 ext4_mount_t *bootstrap=kcalloc(1,sizeof *bootstrap);e4_journal_guard_t *guard=kcalloc(1,sizeof *guard);uint32_t *map=NULL;
 if(!bootstrap||!guard){kfree(bootstrap);kfree(guard);return false;}
 bootstrap->dev=dev;bootstrap->bs=dev->sector_size;bootstrap->blocks=dev->sector_count;bootstrap->journal_bootstrap=true;guard->lifecycle=true;jbd2_source_t source;
 int error=e4_journal_source(bootstrap,guard,&map,&source);if(!error)error=jbd2_analyze(dev,&source,&plan,NULL);
 kfree(map);e4_discard(bootstrap);
 bool visible=false;
 for(unsigned view=0;view<2&&!visible;view++){
  if(view&&!plan)continue;
  bool valid=true;uint64_t offset=physical*bs;
  for(size_t at=0;at<n;){
   uint64_t absolute=offset+at,lba=absolute/dev->sector_size;size_t within=absolute%dev->sector_size,take=dev->sector_size-within;if(take>n-at)take=n-at;
   if(lba>=dev->sector_count||!(view?jbd2_preview_read_sector(plan,lba,sector):dev->read_sector(dev,lba,sector))){valid=false;break;}
   memcpy(bytes+at,sector+within,take);at+=take;
  }
  if(valid){ad_hash(bytes,n,hash);visible=!memcmp(hash,xr_policy.certificate_sha256,32);}
 }
 jbd2_release(plan);kfree(guard);return visible;
}
static bool xr_external_valid(const char *descriptor,const char *rules){
 size_t n,rn;uint8_t *en=load(descriptor,&n),*p=load(rules,&rn);bool ok=rn==1568&&n<=PE_MAX;
 pe_policy policy={0};pe_enrollment verified;
 if(ok){memcpy(policy.keys,p,256);memcpy(policy.uuid,p+256,80);memcpy(policy.descriptor_sha256,p+336,32);
  policy.key_count=e4_u32(p+368);policy.threshold=e4_u32(p+372);policy.epoch=ad_u64(p+376);ok=!pe_verify(en,n,&policy,&verified);}
 free(en);free(p);return ok;
}
int main(int argc,char **argv){
 ng_config();setvbuf(stdout,NULL,_IONBF,0);
 if(argc==5&&!strcmp(argv[1],"setup"))return xr_previous_main(argc,argv);
 assert(argc==7||argc==10);const char *mode=argv[1],*prefix=argv[6];bool activate=!strncmp(mode,"activate",8),inspect=!strcmp(mode,"inspect"),recover=!strncmp(mode,"recover",7)||inspect,reject=!strcmp(mode,"reject");
 assert(!activate||argc==10);size_t size;uint8_t *initial=load(argv[2],&size);unsigned ss=(unsigned)strtoul(argv[3],NULL,10);
 ext4_fault_disk_t d={.stable=malloc(size),.volatile_bytes=malloc(size),.bytes=size,.cut=-1};assert(d.stable&&d.volatile_bytes);ad_disk=&d;
 block_dev_t dev={.sector_size=ss,.sector_count=size/ss,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};
 reset(&d,initial);
 if(!xr_external_valid(argv[4],argv[5])){assert(reject||inspect);assert(!d.events&&!e4_engine_busy);puts("PASS reject external-policy writes=0 flushes=0");free(initial);free(d.stable);free(d.volatile_bytes);return 0;}
 ad_auth(argv[4],argv[5]);
 bool cut=xr_equals(mode)!=NULL,pause=strstr(mode,"pause")!=NULL;
 if(reject){
  const char *stage=NULL;int r=ext4_prepare_root_diagnose(&dev,true,&sv_fs,&stage);
  if(!r)fprintf(stderr,"UNEXPECTED XR acceptance\n");
  assert(r&&!sv_fs&&!e4_engine_busy&&!d.events&&!memcmp(initial,d.stable,size));
  printf("PASS reject stage=%s writes=0 flushes=0\n",stage);
 }else if(activate){
  /* Fixture-only pre-enrollment construction. A required selector on this old
   * image is tested separately and must refuse, never adopt a fallback. */
  xr_active=ad_enabled=ng_required=false;sv_mount(&dev);xr_active=ad_enabled=ng_required=true;
  size_t ln,an,cn,en;uint8_t *l=load(argv[7],&ln),*a=load(argv[8],&an),*c=load(argv[9],&cn),*desc=load(argv[4],&en);
  assert(ln==1024&&an==1024&&cn==PA_BODY+192&&en==PE_BODY+192);size_t n;
  assert(!xr_file(sv_fs->engine,ad_paths[1],xr_new.g,1024,false,&n)&&n==1024);
  assert(!xr_file(sv_fs->engine,ad_paths[2],xr_new.t,8192,false,&n)&&n==8192);
  assert(!xr_file(sv_fs->engine,ad_paths[4],xr_new.o,1024,false,&n)&&n==1024);
  xr_arm(&d,mode);uint64_t irq=spin_lock_irqsave(&e4_lock);xr_typed=xr_bootstrap=true;
  int r=e4_namespace_begin(sv_fs->engine);
  if(!r)r=xr_stage(sv_fs->engine,xr_paths[0],l,ln);
  if(!r)r=xr_stage(sv_fs->engine,ad_paths[5],a,an);
  if(!r)r=xr_stage(sv_fs->engine,xr_paths[1],c,cn);
  if(!r)r=xr_stage(sv_fs->engine,xr_paths[2],desc,en);
  if(!r)r=xr_stage(sv_fs->engine,ad_paths[1],xr_new.g,1024);
  if(!r)r=xr_stage(sv_fs->engine,ad_paths[2],xr_new.t,8192);
  if(!r)r=xr_stage(sv_fs->engine,ad_paths[4],xr_new.o,1024);
  r=e4_namespace_end(sv_fs->engine,r);xr_typed=xr_bootstrap=false;spin_unlock_irqrestore(&e4_lock,irq);
  printf("PASS activation result=%d events=%zu\n",r,d.events);assert((cut||pause)?r==-VFS_EIO:!r);
  if(!r){assert(!xr_account_check());xr_guards();assert(!ext4_freeze_and_sync(sv_fs));}
  save(prefix,r?"interrupted":"final",d.stable,size);free(l);free(a);free(c);free(desc);
 }else{
  if(recover)xr_arm(&d,mode);
  const char *stage=NULL;int r=ext4_prepare_root_diagnose(&dev,true,&sv_fs,&stage);
  if(r){
   if(inspect){assert(!sv_fs&&!e4_engine_busy&&!d.events&&!memcmp(initial,d.stable,size)&&!xr_certificate_visible(&dev));save(prefix,"final",d.stable,size);printf("PASS activation-refused stage=%s writes=0 flushes=0\\n",stage);}
   else{if(!(recover&&cut&&r==-VFS_EIO))fprintf(stderr,"XR mount error=%d stage=%s\n",r,stage);assert(recover&&cut&&r==-VFS_EIO&&!sv_fs&&!e4_engine_busy);save(prefix,"interrupted",d.stable,size);printf("PASS interrupted recovery events=%zu\n",d.events);}
  }
  else{
   ext4_account_namespace_report_t report;assert(!ext4_root_account_namespace_admit(sv_fs,&report));
   int account_error=xr_account_check();if(account_error){
    assert(recover&&cut&&account_error==-VFS_EIO&&d.offline);save(prefix,"interrupted",d.stable,size);
    printf("PASS interrupted account recovery events=%zu authentication-disabled\n",d.events);goto done;
   }
   xr_guards();
   if(!recover){
    int error;vfs_node_t *node=e4_ac_lookup(sv_fs,"/pkg/transaction.v1/artifact.fpk",&error);assert(node&&node->open&&node->close);
    assert(!node->open(node)&&!node->open(node));vfs_node_t *parent=node->parent;assert(parent&&parent->unlink);
    if(!strncmp(mode,"unlink",6))xr_arm(&d,mode);
    xr_typed=true;r=parent->unlink(parent,"artifact.fpk");xr_typed=false;
    if(r){fprintf(stderr,"XR unlink result=%d\n",r);assert(cut||pause);node->close(node);node->close(node);vfs_node_put(node);save(prefix,"interrupted",d.stable,size);printf("PASS interrupted unlink events=%zu\n",d.events);}
    else{
     static uint8_t bytes[8192];assert(node->read(node,0,bytes,xr_new.input.artifact_n)==(int64_t)xr_new.input.artifact_n&&!memcmp(bytes,xr_new.art,xr_new.input.artifact_n));
     size_t events=d.events;node->close(node);assert(events==d.events); /* second open still pins */
     if(!strncmp(mode,"unlink",6)){
      printf("PASS unlink events=%zu\n",d.events);xr_stats();save(prefix,"final",d.stable,size);
      xr_typed=true;node->close(node);xr_typed=false;vfs_node_put(node);
     }else{
      assert(!strncmp(mode,"close",5));xr_arm(&d,mode);xr_typed=true;node->close(node);xr_typed=false;vfs_node_put(node);
      r=sv_fs->engine->tainted?-VFS_EIO:0;assert((cut||pause)?r==-VFS_EIO:!r);
      printf("PASS close result=%d events=%zu\n",r,d.events);
      if(!r){xr_stats();assert(!ext4_freeze_and_sync(sv_fs));}
      save(prefix,r?"interrupted":"final",d.stable,size);
     }
    }
   }else{
    printf("PASS recovery events=%zu\n",d.events);xr_stats();assert(!ext4_freeze_and_sync(sv_fs));save(prefix,"final",d.stable,size);
   }
  }
 }
done:
 xr_active=ad_enabled=ng_required=false;teardown();free(initial);free(d.stable);free(d.volatile_bytes);return 0;
}

static void xr_preview_report(ext4_engine_t *e){uint64_t at=ad_u64(ad_locator(1)+48)*e->fs->bs;bool newer=ad_u64(ad_disk->stable+at+24)!=ad_u64(xr_new.g+24)||memcmp(ad_disk->stable+ad_u64(ad_locator(5)+48)*e->fs->bs+992,xr_new.a+992,32);printf("XR preview-newer=%u\n",newer);xr_previews++;}

static int xr_preflight(ext4_engine_t *e){return xr_bootstrap?0:xr_gate(e,false);}
static int xr_activate(ext4_engine_t *e,unsigned ino,unsigned parent){
 (void)e;return ino==xr_result.binding.inode&&parent==e4_u32(ad_locator(3)+4)?0:-VFS_EROFS;
}
static int xr_retire(ext4_engine_t *e,unsigned ino){
 (void)e;return ino==xr_result.binding.inode?0:-VFS_EROFS;
}

static bool xr_name_allowed(const char *name){for(unsigned i=0;i<4;i++)if(!strcmp(name,xr_paths[i]+9))return true;return false;}
