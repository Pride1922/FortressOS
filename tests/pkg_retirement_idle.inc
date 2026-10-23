#include "crypto.h"
/* Private finite completion profile. Signed hashes authorize exact record sets;
 * actual filesystem identities, ownership and refunds are checked independently.
 * No production caller, signer, trust provisioning or general package executor. */
#define XI_CERT_BODY 1536u
static const char *xi_cert_path="/mnt/pkg/completion.cert";
static uint8_t xi_cert[1728];
static xr_bundle xi_current,xi_next;
static bool xi_manual,xi_terminal_pause;
static unsigned xi_commits,xi_max;
static const char *xi_names[]={"/mnt/pkg/transaction.v1/artifact.fpk","/mnt/usr/bin/demo","/mnt/pkg/transaction.v1/next.v1","/mnt/pkg/transaction.v1/retired.v1"};
static void xi_digest(const xr_bundle *b,uint8_t h[32]){
 digest_t d;digest_init(&d,DIGEST_SHA256);
 assert(digest_update(&d,b->g,1024)&&digest_update(&d,b->t,8192)&&digest_update(&d,b->l,1024)&&digest_update(&d,b->a,1024)&&digest_update(&d,b->o,1024));digest_final(&d,h);
}
static void xi_export(ext4_mount_t *fs,const char *prefix){
 uint32_t ino;assert(!ad_path(fs,xi_cert_path,&ino));e4_inode_t in;assert(!e4_inode(fs,ino,&in));fs->rw_ino=0;assert(!e4_mapping(&in)&&in.map->count<=4);
 uint8_t *p=xi_locator;memset(p,0,128);e4_p16(p,5);e4_p16(p+2,1);e4_p32(p+4,ino);e4_p32(p+8,in.generation);e4_p32(p+12,0600);ad_p64(p+16,in.size);e4_p16(p+32,in.map->count);
 uint64_t blocks=0;for(unsigned i=0;i<in.map->count;i++){e4_extent_t x=in.map->entries[i];e4_p32(p+40+16*i,x.logical);e4_p32(p+44+16*i,x.len);ad_p64(p+48+16*i,x.physical);blocks+=x.len;}ad_p64(p+24,blocks);save(prefix,"completion-locator",p,128);
}
static int xi_certificate(ext4_engine_t *e,bool planned){
 if(!xr_policy.key_count||xr_policy.key_count>8||!xr_policy.threshold||xr_policy.threshold>xr_policy.key_count||e4_u16(xi_locator)!=5||e4_u16(xi_locator+2)!=1||e4_u32(xi_locator+12)!=0600||ad_u64(xi_locator+16)!=sizeof xi_cert||e4_u16(xi_locator+32)>4)return -VFS_EIO;
 for(unsigned i=0;i<xr_policy.key_count;i++){
  bool nonzero=false;for(unsigned j=0;j<32;j++)nonzero|=xr_policy.keys[i][j]!=0;if(!nonzero)return -VFS_EIO;
  for(unsigned j=0;j<i;j++)if(!memcmp(xr_policy.keys[i],xr_policy.keys[j],32))return -VFS_EIO;
 }
 uint32_t ino;uint64_t blocks;size_t n;
 if(ng_file(e,xi_cert_path,xi_cert,sizeof xi_cert,&n,0600,planned,&ino,&blocks)||n!=sizeof xi_cert||ino!=e4_u32(xi_locator+4)||blocks!=ad_u64(xi_locator+24))return -VFS_EIO;
 e4_inode_t in;if(e4_inode(e->fs,ino,&in)||in.generation!=e4_u32(xi_locator+8)||e4_mapping(&in)||in.map->count!=e4_u16(xi_locator+32))return -VFS_EIO;
 for(unsigned i=0;i<in.map->count;i++){e4_extent_t x=in.map->entries[i];const uint8_t *p=xi_locator+40+16*i;if(x.logical!=e4_u32(p)||x.len!=e4_u32(p+4)||x.physical!=ad_u64(p+8)||x.unwritten)return -VFS_EIO;}
 uint8_t h[32];ad_hash(xi_cert,sizeof xi_cert,h);
 if(memcmp(h,xi_pin,32)||memcmp(xi_cert,"FORT-PKG-END-v1\0",16)||e4_u32(xi_cert+16)!=sizeof xi_cert||e4_u16(xi_cert+20)!=1||e4_u16(xi_cert+22)!=14||ad_u64(xi_cert+24)!=xr_en_policy.epoch||memcmp(xi_cert+32,xr_en_policy.uuid,80)||memcmp(xi_cert+112,xr_policy.certificate_sha256,32)||memcmp(xi_cert+144,xr_en_policy.descriptor_sha256,32))return -VFS_EIO;
 for(unsigned i=752;i<768;i++)if(xi_cert[i])return -VFS_EIO;
 for(unsigned i=1048;i<XI_CERT_BODY;i++)if(xi_cert[i])return -VFS_EIO;
 for(unsigned i=0;i<2;i++){
  const uint8_t *s=xi_cert+XI_CERT_BODY+96*i;if(i&&memcmp(s-96,s,32)>=0)return -VFS_EIO;unsigned key=0;
  for(;key<xr_policy.key_count;key++){ad_hash(xr_policy.keys[key],32,h);if(!memcmp(h,s,32))break;}
  if(key==xr_policy.key_count||crypto_ed25519_verify(s+32,xr_policy.keys[key],xi_cert,XI_CERT_BODY))return -VFS_EIO;
 }
 return xr_policy.threshold<=2&&xr_policy.threshold?0:-VFS_EIO;
}
static int xi_load(ext4_engine_t *e,bool planned,xr_bundle *b){
 const char *paths[]={ad_paths[0],ad_paths[1],ad_paths[2],xr_paths[0],ad_paths[5],ad_paths[4],ad_paths[6],ad_paths[7]};uint8_t *buf[]={b->m,b->g,b->t,b->l,b->a,b->o,b->s,b->tr};size_t cap[]={512,1024,8192,1024,1024,1024,1024,1024},n;
 for(unsigned i=0;i<8;i++)if(xr_file(e,paths[i],buf[i],cap[i],planned,&n)||n!=cap[i])return -VFS_EIO;
 return 0;
}
static int xi_identify(const xr_bundle *b,unsigned *stage){
 uint8_t h[32];xi_digest(b,h);for(unsigned i=0;i<12;i++)if(!memcmp(h,xi_cert+304+32*i,32)){*stage=i;return 0;}
 /* After terminal publication only empty account BEGIN/FINISH generations may
  * advance. Normalize exactly those paired fields; all credentials/database
  * hashes and every package byte must still match the signed terminal set. */
 if(e4_u16(b->g+22)!=PG_IDLE||e4_u16(b->a+22)>1)return -VFS_EIO;
 uint64_t seq=ad_u64(b->a+24),base=ad_u64(b->g+24),gen=ad_u64(b->a+32),initial=ad_u64(xi_cert+1040);unsigned open=e4_u16(b->a+22);
 if(gen<initial||seq<base||gen-initial>UINT64_MAX/2||2*(gen-initial)<open||seq-base!=2*(gen-initial)-open)return -VFS_EIO;
 static xr_bundle normalized;memcpy(&normalized,b,sizeof normalized);ad_p64(normalized.a+24,base);ad_p64(normalized.a+32,initial);e4_p16(normalized.a+22,0);ad_p64(normalized.o+24,base);ad_p64(normalized.o+40,initial);ad_seal(normalized.o);ad_hash(normalized.o,1024,normalized.a+112);ad_seal(normalized.a);xi_digest(&normalized,h);
 if(memcmp(h,xi_cert+304+32*11,32))return -VFS_EIO;
 *stage=11;return 0;
}
static int xi_capacity(ext4_engine_t *e,uint64_t left[2]){
 int r=ng_account_left(e,left);if(r)return r;
 for(unsigned i=0;i<e->orphan_count;i++){
  uint32_t ino=e->orphans[i];bool found=false;for(unsigned j=0;j<4;j++)found|=ino==e4_u32(xi_cert+768+64*j+4);if(!found)return -VFS_EIO;
  uint32_t g=(ino-1)/e->fs->ipg,bit=(ino-1)%e->fs->ipg;uint8_t raw[256];if(!e4_bytes(e->fs,(uint64_t)e4_u32(e->fs->gd[g].raw+8)*e->fs->bs+bit*256,raw,256)||e4_u32(raw+28)%(e->fs->bs/512))return -VFS_EIO;
  left[0]+=e4_u32(raw+28)/(e->fs->bs/512);left[1]++;
 }
 return 0;
}
static int xi_native(ext4_engine_t *e,const xr_bundle *b,unsigned stage){
 pm_view names={.grant={.phase=stage>=11?PG_IDLE:PG_RETIRING},.installed=1};if(ng_names_check(e,&names))return -VFS_EIO;
 const char *absent[]={"/mnt/pkg/installed.fpk","/mnt/pkg/transaction.v1/activation","/mnt/pkg/transaction.v1/pending.v1"};
 for(unsigned i=0;i<3;i++){uint32_t ino;if(ad_path(e->fs,absent[i],&ino)!=-VFS_ENOENT)return -VFS_EIO;}
 unsigned orphans=0;uint64_t refund[2]={0,0};static uint8_t bytes[8192];
 for(unsigned i=0;i<4;i++){
  const uint8_t *initial=xi_cert+768+64*i,*row=stage<11?b->t+256+64*i:initial;uint32_t ino=e4_u32(initial+4),named=0;bool promoted=i==1&&stage>=4,dead=stage>=11||e4_u16(row+2)==3;
  if(promoted)dead=false;
  int found=ad_path(e->fs,xi_names[i],&named);if(found&&found!=-VFS_ENOENT)return found;
  if(ino<11||ino>e->fs->inodes)return -VFS_EIO;
  uint32_t g=(ino-1)/e->fs->ipg,bit=(ino-1)%e->fs->ipg;uint8_t raw[256];
  if(!e4_bitmap(e->fs,g,true))return -VFS_EIO;
  bool allocated=e4_bit(e->fs->scratch,bit);
  if(!e4_bytes(e->fs,(uint64_t)e4_u32(e->fs->gd[g].raw+8)*e->fs->bs+bit*256,raw,256))return -VFS_EIO;
  uint32_t generation=e4_u32(raw+100),original=e4_u32(initial+8);
  if(dead){
   if(found!=-VFS_ENOENT)return -VFS_EIO;
   if(stage>=11){if(generation<original||(allocated&&generation==original))return -VFS_EIO;}
   else if(allocated||generation!=original)return -VFS_EIO;
   refund[0]+=ad_u64(initial+40);refund[1]++;continue;
  }
  if(generation!=original)return -VFS_EIO;
  bool orphan=!promoted&&e4_u32(row+12)==2;
  if(!allocated||e4_u16(raw)!=(VFS_S_IFREG|(i==1?0755:0600))||e4_u16(raw+2)||e4_u16(raw+24)||e4_u16(raw+120)||e4_u16(raw+122)||e4_u16(raw+26)!=(orphan?0:1)||(orphan?(found!=-VFS_ENOENT||e4_orphan_index(e,ino)<0):(found||named!=ino)))return -VFS_EIO;
  orphans+=orphan;e4_inode_t in;e->fs->orphan_access=orphan?ino:0;int r=e4_inode(e->fs,ino,&in);e->fs->orphan_access=0;
  uint64_t expected=ad_u64((promoted?initial:row)+40);if(r||in.generation!=e4_u32(initial+8)||in.size!=ad_u64(initial+32)||e4_mapping(&in))return -VFS_EIO;
  uint64_t blocks=0;for(unsigned j=0;j<e->fs->physical_count;j++)blocks+=e->fs->physical[j].len;
  if(blocks!=expected||e4_u32(raw+28)!=blocks*(e->fs->bs/512)||blocks>1||in.map->count>1)return -VFS_EIO;
  refund[0]+=promoted?0:ad_u64(initial+40)-blocks;
  if(blocks){if(in.map->count!=1||in.map->entries[0].logical||in.map->entries[0].len!=1||in.size>sizeof bytes||!e4_bytes(e->fs,(uint64_t)in.map->entries[0].physical*e->fs->bs,bytes,in.size))return -VFS_EIO;
   uint8_t h[32];ad_hash(bytes,in.size,h);const uint8_t *pin=i==0?xi_cert+240:i==1?xi_cert+272:i==2?xi_cert+176:xr_policy.certificate_sha256;
   if(i==3){size_t n;if(xr_file(e,xr_paths[1],bytes,sizeof bytes,false,&n)||n<PA_BODY)return -VFS_EIO;pin=bytes+320;}
   if(memcmp(h,pin,32))return -VFS_EIO;
  }
 }
 if(orphans!=e->orphan_count)return -VFS_EIO;
 static uint8_t sb[1024];if(!e4_bytes(e->fs,1024,sb,1024))return -VFS_EIO;
 if(stage<11)for(unsigned d=0;d<2;d++)if(e4_u32(sb+12+4*d)!=ad_u64(xi_cert+1024+8*d)+refund[d])return -VFS_EIO;
 return 0;
}
static int xi_gate(ext4_engine_t *e,bool planned){
 int r=xi_certificate(e,planned);if(r)return r;r=xr_fixed(e);if(r)return r;r=xi_load(e,planned,&xi_current);if(r)return r;
 static uint8_t retained[8192];size_t retained_n;uint8_t retained_h[32];
 for(unsigned i=1;i<4;i++){
  if(xr_file(e,xr_paths[i],retained,sizeof retained,planned,&retained_n))return -VFS_EIO;
  ad_hash(retained,retained_n,retained_h);const uint8_t *pin=xi_cert+(i==1?112:i==2?144:240);if(memcmp(retained_h,pin,32))return -VFS_EIO;
  if(i==1&&(retained_n<PA_BODY||ad_u64(retained+456)!=ng_base||memcmp(retained+464,ng_platform,32)||ng_policy.now<ad_u64(retained+448)||ng_policy.now<1767225600ULL||ng_policy.now>4102444800ULL))return -VFS_EIO;
 }
 unsigned stage;if(xi_identify(&xi_current,&stage))return -VFS_EIO;
 if(!planned){xi_stage=stage;if(xi_native(e,&xi_current,stage))return -VFS_EIO;}
 pg_binding binding={0};memcpy(binding.uuid,xr_en_policy.uuid,80);memcpy(binding.profile,ad_en.wire+152,32);memcpy(binding.marker_sha256,ad_en.wire+120,32);binding.epoch=xr_en_policy.epoch;
 for(unsigned i=0;i<3;i++){binding.inode[i]=e4_u32(ad_locator(i+1)+4);binding.generation[i]=e4_u32(ad_locator(i+1)+8);}pg_view view;
 if(pg_validate_set(xi_current.m,512,xi_current.g,1024,xi_current.t,8192,&binding,&view))return -VFS_EIO;
 uint8_t h[32];ad_hash(xi_current.s,1024,h);if(memcmp(h,xi_cert+176,32))return -VFS_EIO;ad_hash(xi_current.tr,1024,h);if(memcmp(h,xi_cert+208,32))return -VFS_EIO;
 uint64_t left[2];static uint8_t block[4096];if(xi_capacity(e,left)||ng_block(e,1024/e->fs->bs,block,planned))return -VFS_EIO;
 const uint8_t *sb=block+1024%e->fs->bs;
 for(unsigned i=0;i<2;i++){uint64_t free=e4_u32(sb+12+4*i),reserved=ad_u64(xi_current.g+312+8*i);if(free> (i?e->fs->inodes:e->fs->blocks))return -VFS_EIO;if(free<reserved||free-reserved<left[i])return -VFS_ENOSPC;if(planned&&!xr_typed&&stage<11){uint8_t old[8];if(!e4_bytes(e->fs,1036,old,8)||free!=e4_u32(old+4*i))return -VFS_EROFS;}}
 /* At enrollment the original authority gate must still independently accept. */
 if(stage==0&&xi_archive_gate(e,planned))return -VFS_EIO;
 if(stage>=11){
  static uint8_t account[1024],artifact[8192];size_t n;if(xr_file(e,xr_paths[3],artifact,sizeof artifact,planned,&n))return -VFS_EIO;
  memcpy(account,xi_current.a,1024);e4_p16(account+20,3);memset(account+592,0,400);ad_seal(account);
  pm_input in={.marker=xi_current.m,.ledger=xi_current.g,.owners=xi_current.t,.state=xi_current.s,.trust=xi_current.tr,.artifact=artifact,.artifact_size=n,.account=account,.enrollment=&ad_en,.policy=&ng_policy,.base_version=ng_base,.platform=ng_platform};
  static pm_workspace workspace;pm_view view;if(pm_validate(&in,&workspace,&view)||!view.installed||view.grant.phase!=PG_IDLE)return -VFS_EIO;
 }
 return 0;
}
static int xr_gate(ext4_engine_t *e,bool planned){return xi_gate(e,planned);}
static unsigned xi_victim(void){return xi_stage<3?0:xi_stage>=4&&xi_stage<7?2:xi_stage>=7&&xi_stage<10?3:4;}
static int xi_activate(unsigned ino,unsigned parent){unsigned i=xi_victim();return i<4&&ino==e4_u32(xi_cert+772+64*i)&&parent==e4_u32(ad_locator(3)+4)?0:-VFS_EROFS;}
static int xi_retire(unsigned ino){unsigned i=xi_victim();return i<4&&ino==e4_u32(xi_cert+772+64*i)?0:-VFS_EROFS;}
static void xi_build(xr_bundle *b,unsigned next){
 if(next==11){e4_p16(b->g+22,PG_IDLE);memset(b->g+40,0,16);memset(b->g+136,0,32);memcpy(b->g+168,b->g+200,32);memset(b->g+296,0,48);e4_p16(b->g+348,0);memcpy(b->g+360,b->g+368,8);memset(b->g+392,0,40);e4_p16(b->t+22,0);memset(b->t+32,0,16);memset(b->t+256,0,7904);}
 ad_hash(b->t,8160,b->t+8160);ad_hash(b->t,8192,b->g+264);ad_seal(b->g);
 if(next==11)memset(b->l,0,1024);else{memcpy(b->l+240,b->g+296,48);xr_build(b);}
 ad_p64(b->o+24,ad_u64(b->a+24));ad_p64(b->o+40,ad_u64(b->a+32));ad_seal(b->o);ad_hash(b->o,1024,b->a+112);memcpy(b->a+440,b->g+24,8);ad_hash(b->g,1024,b->a+448);memcpy(b->a+480,b->g+232,32);memcpy(b->a+512,b->g+200,32);memcpy(b->a+544,b->g+136,32);memcpy(b->a+576,b->g+40,16);if(next==11)memset(b->a+592,0,32);else ad_hash(b->l,1024,b->a+592);ad_seal(b->a);
}
static int xi_commit(ext4_engine_t *e){
 if(!xr_active)return xi_archive_commit(e);
 if(xr_bootstrap)return xi_archive_commit(e);
 if(!xr_typed){int r=xi_gate(e,true);if(!r)r=xi_fence(e);return r?r:ad_core_commit(e);}
 if(xi_gate(e,false))return -VFS_EIO;
 unsigned before=xi_stage,next=before+1;if(next>11)return -VFS_EROFS;
 memcpy(&xi_next,&xi_current,sizeof xi_next);xr_bundle *b=&xi_next;
 ad_p64(b->g+24,ad_u64(b->g+24)+1);ad_p64(b->a+24,ad_u64(b->a+24)+1);memcpy(b->t+48,b->g+24,8);
 if(next==4){if(!xi_manual||e->orphan_count)return -VFS_EIO;uint8_t *row=b->t+320;for(unsigned d=0;d<2;d++){uint64_t n=ad_u64(row+40+8*d);ad_p64(b->g+296+8*d,ad_u64(b->g+296+8*d)-n);ad_p64(b->g+328+8*d,ad_u64(b->g+328+8*d)-n);}e4_p16(row+2,3);memset(row+12,0,4);memset(row+32,0,24);}
 else if(next==11){if(!xi_manual||e->orphan_count||ad_u64(b->g+328)||ad_u64(b->g+336))return -VFS_EIO;}
 else{
  if(xi_manual)return -VFS_EIO;
  unsigned i=xi_victim();if(i>=4)return -VFS_EIO;uint8_t *row=b->t+256+64*i;uint32_t ino=e4_u32(row+4),g=(ino-1)/e->fs->ipg,bit=(ino-1)%e->fs->ipg;uint8_t *map=e4_stage_bitmap(e,g,true),*raw=e4_inode_image(e,ino);if(!map||!raw)return -VFS_EIO;
  bool allocated=e4_bit(map,bit);uint64_t owned[2]={allocated?e4_u32(raw+28)/(e->fs->bs/512):0,allocated?1:0};static uint8_t oldsb[1024];if(!e4_bytes(e->fs,1024,oldsb,1024))return -VFS_EIO;uint8_t *sb=e4_super(e);if(!sb)return -VFS_EIO;
  for(unsigned d=0;d<2;d++){uint64_t old=ad_u64(row+40+8*d);if(owned[d]>old||e4_u32(sb+12+4*d)<e4_u32(oldsb+12+4*d)||e4_u32(sb+12+4*d)-e4_u32(oldsb+12+4*d)!=old-owned[d])return -VFS_EIO;ad_p64(b->g+312+8*d,ad_u64(b->g+312+8*d)+old-owned[d]);ad_p64(b->g+328+8*d,ad_u64(b->g+328+8*d)-old+owned[d]);ad_p64(row+40+8*d,owned[d]);}
  e4_p16(row+2,allocated?1:3);e4_p32(row+12,allocated?(e4_u16(raw+26)?1:2):0);if(!allocated)memset(row+32,0,8);
 }
 xi_build(b,next);unsigned identified;if(xi_identify(b,&identified)||identified!=next)return -VFS_EIO;
 const char *paths[]={ad_paths[1],ad_paths[2],xr_paths[0],ad_paths[5],ad_paths[4]};const uint8_t *bytes[]={b->g,b->t,b->l,b->a,b->o};size_t sizes[]={1024,8192,1024,1024,1024};
 for(unsigned i=0;i<5;i++)if(xr_stage(e,paths[i],bytes[i],sizes[i]))return -VFS_EIO;
 if(xi_gate(e,true))return -VFS_EIO;
 if(e->images>xi_max)xi_max=e->images;
 int r=ad_core_commit(e);if(!r)xi_commits++;return r;
}
static int e4_semantic_commit(ext4_engine_t *e){return xi_commit(e);}
static int xi_fence(ext4_engine_t *e){
 bool pair=ad_pair_staged&&xi_stage>=11;
 uint32_t ids[32];unsigned count=0;
 for(unsigned i=0;i<9;i++)ids[count++]=e4_u32(ad_locator(i)+4);
 ids[count++]=e4_u32(ad_locator(14)+4);
 for(unsigned i=0;i<4;i++)ids[count++]=e4_u32(xr_locations+128*i+4);
 ids[count++]=e4_u32(xi_locator+4);
 for(unsigned i=0;i<4;i++){
  bool live=i==1||(xi_stage<11&&e4_u16(xi_current.t+258+64*i)!=3);
  if(live)ids[count++]=e4_u32(xi_cert+772+64*i);
 }
 for(unsigned i=0;i<count;i++){
  uint32_t ino=ids[i];e4_inode_t in;bool orphan=e4_orphan_index(e,ino)>=0;e->fs->orphan_access=orphan?ino:0;int r=e4_inode(e->fs,ino,&in);e->fs->orphan_access=0;if(r)return r;
  e->fs->rw_ino=0;if(e4_mapping(&in))return -VFS_EIO;
  bool writable=pair&&(ino==e4_u32(ad_locator(4)+4)||ino==e4_u32(ad_locator(5)+4));
  if(!writable)for(unsigned j=0;j<e->images;j++)for(unsigned k=0;k<e->fs->physical_count;k++)if(e->image[j].block>=e->fs->physical[k].lo&&(uint64_t)e->image[j].block<e->fs->physical[k].lo+e->fs->physical[k].len)return -VFS_EROFS;
  uint32_t g=(ino-1)/e->fs->ipg,bit=(ino-1)%e->fs->ipg;uint64_t at=(uint64_t)e4_u32(e->fs->gd[g].raw+8)*e->fs->bs+bit*256;uint8_t raw[256];if(!e4_bytes(e->fs,at,raw,256))return -VFS_EIO;
  for(unsigned j=0;j<e->images;j++)if(e->image[j].block==at/e->fs->bs&&memcmp(raw,e->image[j].bytes+at%e->fs->bs,256))return -VFS_EROFS;
 }
 return 0;
}
static int xi_manual_step(ext4_engine_t *e){uint64_t irq=spin_lock_irqsave(&e4_lock);xr_typed=xi_manual=true;if(xi_terminal_pause&&xi_stage==10)ng_pause=true;int r=e4_namespace_begin(e);if(!r)r=e4_namespace_end(e,0);xr_typed=xi_manual=false;spin_unlock_irqrestore(&e4_lock,irq);return r;}
static int xi_finish(ext4_engine_t *e){
 for(unsigned steps=0;steps<16;steps++){
  uint64_t irq=spin_lock_irqsave(&e4_lock);int r=ad_validate(e);spin_unlock_irqrestore(&e4_lock,irq);if(r)return r;
  if(xi_stage>=11)return 0;
  if(xi_stage==3||xi_stage==10){r=xi_manual_step(e);if(r)return r;continue;}
  unsigned victim=xi_victim();if(victim>=4)return -VFS_EIO;int err;vfs_node_t *n=e4_ac_lookup(sv_fs,xi_names[victim]+4,&err);
  if(n){xr_typed=true;r=n->parent->unlink(n->parent,n->name);xr_typed=false;vfs_node_put(n);if(r)return r;}
  irq=spin_lock_irqsave(&e4_lock);xr_typed=true;r=ext4_engine_orphan_recover(e);xr_typed=false;spin_unlock_irqrestore(&e4_lock,irq);if(r)return r;
 }
 return -VFS_EIO;
}
static void xi_pressure(ext4_engine_t *e){
 size_t events=ad_disk->events;uint64_t irq=spin_lock_irqsave(&e4_lock);vfs_create_attrs_t attrs={.mode=0600};uint32_t ino=0;int r=ext4_engine_namespace_create_attrs(e,2,"idle-competing",false,&attrs,&ino);spin_unlock_irqrestore(&e4_lock,irq);assert(r==-VFS_ENOSPC&&ad_disk->events==events&&!e->tainted);
 int error;vfs_node_t *n=e4_ac_lookup(sv_fs,"/pkg-competing",&error);assert(n);uint64_t off=sv_fs->bs;uint8_t byte='X';assert(n->write(n,&off,false,&byte,1)==-VFS_ENOSPC&&ad_disk->events==events&&!e->tainted);vfs_node_put(n);puts("PASS competing block/inode allocations refused before I/O");
}
static void xi_ordinary(void){
 uint64_t irq=spin_lock_irqsave(&e4_lock);vfs_create_attrs_t attrs={.mode=0600};uint32_t ino;assert(!ext4_engine_namespace_create_attrs(sv_fs->engine,2,"after-idle",false,&attrs,&ino));spin_unlock_irqrestore(&e4_lock,irq);
 int error;vfs_node_t *n=e4_ac_lookup(sv_fs,"/after-idle",&error);assert(n);uint64_t off=0;const uint8_t bytes[]="fresh baseline allocation";assert(n->write(n,&off,false,bytes,sizeof bytes)==sizeof bytes);vfs_node_put(n);uint64_t lock=spin_lock_irqsave(&e4_lock);e4_inode_t in;assert(!e4_inode(sv_fs,ino,&in));bool reused=false;for(unsigned i=0;i<4;i++)if(i!=1&&ino==e4_u32(xi_cert+772+64*i)){assert(in.generation>e4_u32(xi_cert+776+64*i));reused=true;}spin_unlock_irqrestore(&e4_lock,lock);assert(reused);puts("PASS ordinary inode and block allocation after IDLE; retired inode reused with newer generation");
}
int main(int argc,char **argv){
 if(argc==5||!strncmp(argv[1],"activate",8))return xi_previous_main(argc,argv);
 ng_config();setvbuf(stdout,NULL,_IONBF,0);assert(argc==7);const char *mode=argv[1],*prefix=argv[6];size_t size;uint8_t *initial=load(argv[2],&size);unsigned ss=strtoul(argv[3],NULL,10);
 ext4_fault_disk_t d={.stable=malloc(size),.volatile_bytes=malloc(size),.bytes=size,.cut=-1};ad_disk=&d;block_dev_t dev={.sector_size=ss,.sector_count=size/ss,.read_sector=ext4_fault_read,.write_sector=ext4_fault_write,.flush=ext4_fault_flush,.priv=&d};reset(&d,initial);ad_auth(argv[4],argv[5]);
 bool cut=xr_equals(mode)!=NULL||!strcmp(mode,"terminal-pause");xr_arm(&d,mode);xi_terminal_pause=!strcmp(mode,"terminal-pause");if(xi_terminal_pause)ng_pause=false;const char *where=NULL;int r=ext4_prepare_root_diagnose(&dev,true,&sv_fs,&where);
 if(!strcmp(mode,"reject")){assert(r&&!sv_fs&&!e4_engine_busy&&!d.events&&!memcmp(initial,d.stable,size));printf("PASS reject %s writes=0 flushes=0\n",where);goto done;}
 if(!r&&!strcmp(mode,"pressure"))xi_pressure(sv_fs->engine);
 if(!r)r=xi_finish(sv_fs->engine);
 if(!r)r=accounts_prepare(sv_fs);
 if(r&&cut){assert((!enabled&&!writer)||(sv_fs&&sv_fs->engine->tainted)||!sv_fs);}
 if(!r&&!strcmp(mode,"ordinary"))xi_ordinary();
 if(!r&&strncmp(mode,"recover",7)==0){assert(xi_stage>=11);}
 if(!r&&!strcmp(mode,"fresh")){
  assert(enabled);creds_t root;creds_init_root(&root);account_request_t req={.size=64,.version=1};for(unsigned lifetime=0;lifetime<3;lifetime++){req=(account_request_t){.size=64,.version=1};assert(!accounts_control(ACCOUNT_BEGIN,(uintptr_t)&req,sizeof req,&root,77)&&writer==77&&req.token);
  size_t events=d.events;account_request_t second={.size=64,.version=1};assert(accounts_control(ACCOUNT_BEGIN,(uintptr_t)&second,sizeof second,&root,88)==SYSCALL_EAGAIN&&d.events==events);
  assert(accounts_control(ACCOUNT_FINISH,(uintptr_t)&req,sizeof req,&root,88)!=0&&d.events==events&&writer==77);assert(!accounts_control(ACCOUNT_FINISH,(uintptr_t)&req,sizeof req,&root,77)&&!writer);}puts("PASS fresh account writer; competing owner refused; three owner finishes durable");
 }
 if(!r&&!strcmp(mode,"death")){
  creds_t root;creds_init_root(&root);account_request_t req={.size=64,.version=1};assert(!accounts_control(ACCOUNT_BEGIN,(uintptr_t)&req,sizeof req,&root,77));size_t events=d.events;accounts_process_exit(88);assert(writer==77&&d.events==events);accounts_process_exit(77);assert(!writer&&!enabled&&d.events==events);account_request_t retry={.size=64,.version=1};assert(accounts_control(ACCOUNT_BEGIN,(uintptr_t)&retry,sizeof retry,&root,88)==SYSCALL_EAGAIN&&d.events==events);puts("PASS process-owner exit fences admission until durable recovery");
 }
 if(r){if(!cut)fprintf(stderr,"XI failure r=%d where=%s stage=%u events=%zu\n",r,where?where:"operation",xi_stage,d.events);assert(cut&&r==-VFS_EIO);save(prefix,"interrupted",d.stable,size);printf("PASS interrupted events=%zu stage=%u\n",d.events,xi_stage);}
 else{uint64_t irq=spin_lock_irqsave(&e4_lock);assert(!ad_validate(sv_fs->engine));spin_unlock_irqrestore(&e4_lock,irq);assert(xi_stage>=11);printf("PASS IDLE events=%zu stage=%u commits=%u images=%u\n",d.events,xi_stage,xi_commits,xi_max);assert(!ext4_freeze_and_sync(sv_fs));save(prefix,"final",d.stable,size);}
 done:xr_active=ad_enabled=ng_required=false;teardown();free(initial);free(d.stable);free(d.volatile_bytes);return 0;
}
