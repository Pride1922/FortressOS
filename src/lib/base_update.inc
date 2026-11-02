#include "base_update.h"
#include "install_archive.h"
static int bsha(const void *p,size_t n,uint8_t out[32]){
    digest_t d;digest_init(&d,DIGEST_SHA256);(void)digest_update(&d,p,n);digest_final(&d,out);return 0;
}
static void bput(uint8_t *p,uint64_t v,unsigned n){for(unsigned i=0;i<n;i++)p[i]=(uint8_t)(v>>(8*i));}
void base_state_encode(const base_state *s,uint8_t p[512]){
    memset(p,0,512);memcpy(p,"FOSBST01",8);bput(p+8,s->phase,4);bput(p+12,s->slot,4);
    bput(p+16,s->build,8);bput(p+24,s->abi,8);bput(p+32,s->catalog,8);bput(p+40,s->root,8);
    memcpy(p+48,s->platform,32);memcpy(p+80,s->catalog_hash,32);memcpy(p+112,s->root_hash,32);
    memcpy(p+144,s->artifact,32);memcpy(p+176,s->kernel_hash,32);memcpy(p+208,s->bootstrap_hash,32);
    memcpy(p+240,s->readiness_hash,32);bput(p+272,s->generation,8);(void)bsha(p,480,p+480);
}
int base_state_decode(const uint8_t p[512],base_state *out){
    uint8_t sum[32];
    if(memcmp(p,"FOSBST01",8)||u32(p+8)<1||u32(p+8)>4||u32(p+12)>1||!u64(p+16)||!u64(p+24)||
       zero(p+48,32)||!zero(p+280,200)||bsha(p,480,sum)||memcmp(sum,p+480,32)||
       (u64(p+32)?(!u64(p+40)||zero(p+80,32)||zero(p+112,32)||zero(p+144,32)):
        (u64(p+40)||!zero(p+80,96))))return -1;
    *out=(base_state){.phase=u32(p+8),.slot=u32(p+12),.build=u64(p+16),.abi=u64(p+24),.catalog=u64(p+32),.root=u64(p+40),.generation=u64(p+272)};
    memcpy(out->platform,p+48,32);memcpy(out->catalog_hash,p+80,32);memcpy(out->root_hash,p+112,32);
    memcpy(out->artifact,p+144,32);memcpy(out->kernel_hash,p+176,32);memcpy(out->bootstrap_hash,p+208,32);
    memcpy(out->readiness_hash,p+240,32);return 0;
}
size_t base_update_menu(char *b,size_t cap,const char *uuid,unsigned active){
    gpt_guid_t guid;if(!b||!uuid||active>1||!gpt_str_to_guid(uuid,&guid))return 0;
    size_t n=0;
    if(!append(b,cap,&n,active?"default_entry: 3\ntimeout: 5\n":"default_entry: 1\ntimeout: 5\n"))return 0;
    for(unsigned bank=0;bank<2;bank++)for(unsigned recovery=0;recovery<2;recovery++){
        if(!append(b,cap,&n,bank?"/FortressOS base B":"/FortressOS base A")||
           !append(b,cap,&n,recovery?" authenticated recovery\n":" installed\n")||
           !append(b,cap,&n,"    protocol: limine\n    kernel_path: boot():/boot/")||
           !append(b,cap,&n,bank?"kernel-b.elf":"fortress.elf")||
           !append(b,cap,&n,"\n    kernel_cmdline: base_layout=1 boot_mode=")||
           !append(b,cap,&n,recovery?"recovery":"installed")||!append(b,cap,&n," root=PARTUUID=")||!append(b,cap,&n,uuid)||
           !append(b,cap,&n,"\n    module_path: boot():/boot/")||!append(b,cap,&n,bank?"bootstrap-b.tar":"bootstrap.tar")||
           !append(b,cap,&n,"\n    module_cmdline: role=bootstrap\n    module_path: boot():/boot/")||
           !append(b,cap,&n,bank?"ready-b.v1":"readiness.v1")||!append(b,cap,&n,"\n    module_cmdline: role=readiness\n"))return 0;
    }return n;
}
/* A torn confirmation changes only phase and checksum. Recognize exactly the
 * alternate valid checksum, retain READY, and never infer confirmation. */
static int base_boot_state_decode(const uint8_t p[512],base_state *s){
    if(!base_state_decode(p,s))return 0;
    unsigned phase=u32(p+8);if(phase!=BASE_STATE_READY&&phase!=BASE_STATE_CONFIRMED)return -1;
    uint8_t previous[512];memcpy(previous,p,512);bput(previous+8,phase==BASE_STATE_READY?BASE_STATE_CONFIRMED:BASE_STATE_READY,4);
    if(base_state_decode(previous,s))return -1;
    s->phase=BASE_STATE_READY;return 0;
}
static int base_offsets(uint8_t *b,base_update_work *w){
    install_fat_work_t *f=&w->disk.fat;
    if(install_fat_complete(f)!=1||f->cluster_bytes!=512||
       !extent(b,f,f->kernel_first,f->kernel_size,&w->disk.kernel[0])||
       !extent(b,f,f->bank_first,f->bank_size,&w->disk.kernel[1])||
       !extent(b,f,f->first[0],BASE_BANK_BYTES,&w->bootstrap[0])||
       !extent(b,f,f->bootstrap_b_first,BASE_BANK_BYTES,&w->bootstrap[1])||
       !extent(b,f,f->first[1],512,&w->disk.readiness[0])||
       !extent(b,f,f->ready_b_first,512,&w->disk.readiness[1]))return -1;
    for(unsigned i=0;i<2;i++)if(!extent(b,f,f->base_first[i],512,&w->state_offset[i])||!extent(b,f,f->floor_first[i],512,&w->floor_offset[i]))return -1;
    for(unsigned i=0;i<4;i++)if(!extent(b,f,f->first[i+2],f->capacity[i+2],&w->disk.configs[i]))return -1;
    return 0;
}
int base_update_seed(void *bytes,size_t n,base_update_work *w,uint64_t build,uint64_t abi,const uint8_t platform[32],const uint8_t ready[512]){
    if(!bytes||!w||!platform||!ready||n!=BASE_ESP_BYTES||!build||!abi||zero(platform,32)||
       !disjoint(bytes,n,w,sizeof *w)||!disjoint(bytes,n,ready,512))return -1;
    memset(w,0,sizeof *w);uint8_t *b=bytes;
    if(install_fat_inspect(b,n,&w->disk.fat)||base_offsets(b,w))return -1;
    for(unsigned i=0;i<2;i++){
        base_state *s=w->states+i;*s=(base_state){.build=build,.abi=abi,.phase=BASE_STATE_CONFIRMED,.slot=i};memcpy(s->platform,platform,32);
        (void)bsha(b+w->disk.kernel[i],BASE_BANK_BYTES,s->kernel_hash);
        (void)bsha(b+w->bootstrap[i],BASE_BANK_BYTES,s->bootstrap_hash);
        (void)bsha(ready,512,s->readiness_hash);
        base_state_encode(s,b+w->state_offset[i]);
        w->floors[i]=*s;w->floors[i].phase=BASE_STATE_FLOOR;w->floors[i].generation=1;
        memset(w->floors[i].kernel_hash,0,96);base_state_encode(w->floors+i,b+w->floor_offset[i]);
    }return 0;
}
static bool base_matches(uint8_t *b,base_update_work *w,unsigned i){
    base_state *s=w->states+i;uint8_t sum[32];root_recovery_t r={0};
    if(s->phase==BASE_STATE_PREPARING)return false;
    identity(b+w->disk.readiness[i],&r);
    if(!root_readiness_validate(b+w->disk.readiness[i],512,&r,b+w->disk.kernel[i],BASE_BANK_BYTES,b+w->bootstrap[i],BASE_BANK_BYTES))return false;
    (void)bsha(b+w->disk.kernel[i],BASE_BANK_BYTES,sum);if(memcmp(sum,s->kernel_hash,32))return false;
    (void)bsha(b+w->bootstrap[i],BASE_BANK_BYTES,sum);if(memcmp(sum,s->bootstrap_hash,32))return false;
    (void)bsha(b+w->disk.readiness[i],512,sum);return !memcmp(sum,s->readiness_hash,32);
}
int base_update_prepare(const kernel_update_io_t *io,void *bytes,base_update_work *w,const uint8_t running[512]){
    if(!io||!io->read||!io->write||!io->flush||!bytes||!w||!running||
       !disjoint(bytes,BASE_ESP_BYTES,w,sizeof *w)||!disjoint(bytes,BASE_ESP_BYTES,io,sizeof *io)||
       !disjoint(bytes,BASE_ESP_BYTES,running,512)||!disjoint(w,sizeof *w,running,512))return -1;
    memset(w,0,sizeof *w);w->running=2;uint8_t *b=bytes;
    if(disk(io,&w->disk)||read_runs(io,w->disk.esp,b,BASE_ESP_BYTES/512)||
       install_fat_inspect(b,BASE_ESP_BYTES,&w->disk.fat)||base_offsets(b,w))return -1;
    for(unsigned i=0;i<2;i++){
        if(base_boot_state_decode(b+w->state_offset[i],w->states+i)||w->states[i].slot!=i||w->states[i].phase==BASE_STATE_FLOOR||w->states[i].generation){w->invalid|=1u<<i;continue;}
        if(!memcmp(b+w->disk.readiness[i],running,512)&&base_matches(b,w,i)){
            if(w->running!=2 && (memcmp(b+w->disk.kernel[i],b+w->disk.kernel[w->running],BASE_BANK_BYTES)||
                memcmp(b+w->bootstrap[i],b+w->bootstrap[w->running],BASE_BANK_BYTES)))return -1;
            w->running=i;
        }
    }
    if(w->running==2)return -1;
    for(unsigned i=0;i<2;i++){
        base_state *s=w->floors+i;
        if(!base_state_decode(b+w->floor_offset[i],s)&&s->phase==BASE_STATE_FLOOR&&s->slot==i&&s->generation&&
           zero(s->kernel_hash,96)&&s->abi==w->states[w->running].abi&&!memcmp(s->platform,w->states[w->running].platform,32))w->floor_valid|=1u<<i;
    }
    if(!w->floor_valid)return -1;
    w->floor=(w->floor_valid&1)?0:1;
    if(w->floor_valid==3){
        if(w->floors[1].generation>w->floors[0].generation)w->floor=1;
        if(w->floors[1].generation==w->floors[0].generation){
            base_state a=w->floors[0],c=w->floors[1];a.slot=c.slot=0;
            if(memcmp(&a,&c,sizeof a))return -1;
        }
    }
    for(unsigned i=0;i<2;i++)if(!(w->invalid&(1u<<i))&&w->states[i].build>w->floors[w->floor].build)return -1;
    root_recovery_t r={0};identity(running,&r);gpt_guid_t guid;
    if(!gpt_str_to_guid(r.partuuid,&guid)||memcmp(guid.bytes,w->disk.entries+128+16,16)||
       io->read(io->context,w->disk.root+2,w->disk.verify,2)||w->disk.verify[56]!=0x53||w->disk.verify[57]!=0xef||
       !gpt_str_to_guid(r.filesystem_uuid,&guid))return -1;
    const uint8_t *g=guid.bytes;uint8_t fsuuid[16]={g[3],g[2],g[1],g[0],g[5],g[4],g[7],g[6],g[8],g[9],g[10],g[11],g[12],g[13],g[14],g[15]};
    if(memcmp(w->disk.verify+104,fsuuid,16))return -1;
    for(unsigned i=0;i<4;i++){
        char *menu=(char *)b+w->disk.configs[i];unsigned selected=menu[15]=='3';
        size_t n=base_update_menu((char *)w->disk.entries,sizeof w->disk.entries,r.partuuid,selected);
        if((menu[15]!='1'&&menu[15]!='3')||!n||w->disk.fat.capacity[i+2]!=n||memcmp(menu,w->disk.entries,n))return -1;
        if(!i)w->disk.active=selected;else if(selected!=w->disk.active)w->mixed=true;
        w->disk.config_sizes[i]=(uint32_t)n;
    }
    /* Identical initial banks are interchangeable; use the selected one. */
    if(!(w->invalid&(1u<<w->disk.active))&&!memcmp(b+w->disk.readiness[w->disk.active],running,512)&&base_matches(b,w,w->disk.active))w->running=w->disk.active;
    memcpy(w->disk.ready,running,512);w->disk.prepared=true;return 0;
}
static int base_publish(const kernel_update_io_t *io,base_update_work *w,uint32_t offset,const uint8_t *p,unsigned bytes){
    if((offset|bytes)&511||offset>BASE_ESP_BYTES||bytes>BASE_ESP_BYTES-offset)return -1;
    w->wrote=true;
    uint64_t lba=w->disk.esp+offset/512;
    for(unsigned at=0;at<bytes;at+=4096){unsigned take=bytes-at;if(take>4096)take=4096;if(io->write(io->context,lba+at/512,p+at,take/512))return -1;}
    if(io->flush(io->context))return -1;
    for(unsigned at=0;at<bytes;at+=4096){unsigned take=bytes-at;if(take>4096)take=4096;if(io->read(io->context,lba+at/512,w->disk.verify,take/512)||memcmp(w->disk.verify,p+at,take))return -1;}
    return 0;
}
static int base_select(const kernel_update_io_t *io,uint8_t *b,base_update_work *w,unsigned slot){
    for(unsigned i=0;i<4;i++){uint8_t *menu=b+w->disk.configs[i];menu[15]=slot?'3':'1';if(base_publish(io,w,w->disk.configs[i],menu,512))return -1;}return 0;
}
int base_update_select(const kernel_update_io_t *io,void *bytes,base_update_work *w,bool recover){
    if(!io||!bytes||!w||!w->disk.prepared||(!recover&&w->invalid)||(!recover&&(w->mixed||w->running!=w->disk.active)))return -1;
    w->disk.prepared=false;uint8_t *b=bytes;
    if(recover && w->invalid){
        unsigned slot=w->running^1;base_state repair=w->floors[w->floor];repair.slot=slot;repair.generation=0;repair.phase=BASE_STATE_PREPARING;
        base_state_encode(&repair,b+w->state_offset[slot]);
        if(base_publish(io,w,w->state_offset[slot],b+w->state_offset[slot],512))return -1;
    }
    if(recover&&base_select(io,b,w,w->running))return -1;
    base_state *s=w->states+w->running;s->phase=BASE_STATE_CONFIRMED;base_state_encode(s,b+w->state_offset[w->running]);
    return base_publish(io,w,w->state_offset[w->running],b+w->state_offset[w->running],512);
}
int base_update_apply(const kernel_update_io_t *io,void *bytes,base_update_work *w,const base_artifact *a,const base_state *release,const void *recovery,size_t rn){
    if(!io||!bytes||!w||!a||!release||!recovery||!w->disk.prepared||w->invalid||w->mixed||w->running!=w->disk.active||
       w->states[w->running].phase!=BASE_STATE_CONFIRMED||release->build!=a->build||release->abi!=a->abi||
       w->states[w->running].build<a->predecessor_min||w->states[w->running].build>a->predecessor_max||
       memcmp(a->platform,w->states[w->running].platform,32)||a->abi!=w->states[w->running].abi)return -1;
    base_state *floor=&w->floors[w->floor];
    if(a->build<floor->build||(a->build==floor->build&&memcmp(release->artifact,floor->artifact,32))||
       release->catalog<floor->catalog||(release->catalog==floor->catalog&&memcmp(release->catalog_hash,floor->catalog_hash,32))||
       release->root<floor->root||(release->root==floor->root&&memcmp(release->root_hash,floor->root_hash,32))||floor->generation==UINT64_MAX)return -1;
    w->disk.prepared=false;uint8_t *b=bytes;unsigned slot=w->running^1;
    for(unsigned i=0;i<2;i++)if(a->build<=w->states[i].build &&
       !(a->build==w->states[i].build && (w->states[i].phase==BASE_STATE_PREPARING || w->states[i].phase==BASE_STATE_READY) &&
         !memcmp(release->artifact,w->states[i].artifact,32)))return -1;
    uint8_t *boot=b+w->bootstrap[slot];size_t written=0;
    if(install_archive_personalize(a->bootstrap,a->bootstrap_bytes,recovery,rn,boot,BASE_BANK_BYTES,&written))return -1;
    uint8_t *kernel=b+w->disk.kernel[slot];memset(kernel,0,BASE_BANK_BYTES);memcpy(kernel,a->kernel,a->kernel_bytes);
    base_state s=*release;s.slot=slot;s.phase=BASE_STATE_PREPARING;
    (void)bsha(kernel,BASE_BANK_BYTES,s.kernel_hash);(void)bsha(boot,BASE_BANK_BYTES,s.bootstrap_hash);
    char *ready=(char *)w->disk.ready,*kh=strstr(ready,"\nkernel_sha256="),*bh=strstr(ready,"\nbootstrap_sha256="),*cs=strstr(ready,"\nchecksum=");
    if(!kh||!bh||!cs||cs-ready>440)return -1;
    char sum[65];hash(kernel,BASE_BANK_BYTES,sum);memcpy(kh+15,sum,64);hash(boot,BASE_BANK_BYTES,sum);memcpy(bh+18,sum,64);
    hash(ready,(size_t)(cs+1-ready),sum);memcpy(cs+10,sum,64);
    root_recovery_t r={0};identity(w->disk.ready,&r);
    if(!root_readiness_validate(ready,512,&r,kernel,BASE_BANK_BYTES,boot,BASE_BANK_BYTES))return -1;
    (void)bsha(ready,512,s.readiness_hash);base_state_encode(&s,b+w->state_offset[slot]);
    /* Publish a new floor into the other record. A torn floor write always
     * leaves the previous highest valid generation intact. */
    unsigned fi=w->floor^1;base_state next_floor=*release;next_floor.slot=fi;next_floor.phase=BASE_STATE_FLOOR;
    next_floor.generation=floor->generation+1;memset(next_floor.kernel_hash,0,96);
    base_state_encode(&next_floor,b+w->floor_offset[fi]);
    if(base_publish(io,w,w->floor_offset[fi],b+w->floor_offset[fi],512)||
       base_publish(io,w,w->state_offset[slot],b+w->state_offset[slot],512)||
       base_publish(io,w,w->disk.kernel[slot],kernel,BASE_BANK_BYTES)||base_publish(io,w,w->bootstrap[slot],boot,BASE_BANK_BYTES)||
       base_publish(io,w,w->disk.readiness[slot],(uint8_t *)ready,512))return -1;
    s.phase=BASE_STATE_READY;base_state_encode(&s,b+w->state_offset[slot]);
    if(base_publish(io,w,w->state_offset[slot],b+w->state_offset[slot],512))return -1;
    return base_select(io,b,w,slot);
}
