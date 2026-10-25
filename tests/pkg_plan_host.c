#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "pkg_plan_candidate.h"
#include "crypto.h"
#include "string.h"
typedef struct {uint8_t *p;size_t n,at;} reader;
static long read_bytes(void *context,void *out,size_t n){reader *r=context;if(n>r->n-r->at)n=r->n-r->at;if(n>37)n=37;memcpy(out,r->p+r->at,n);r->at+=n;return (long)n;}
static void load(const char *path,reader *r){FILE *f=fopen(path,"rb");assert(f&&!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>0&&!fseek(f,0,SEEK_SET));r->p=malloc(n);assert(r->p&&fread(r->p,1,n,f)==(size_t)n&&!fclose(f));r->n=n;r->at=0;}
static pi_workspace w;static pi_plan plan,saved;static pi_inventory inventory;static pi_object objects[16];static pi_resources resources;
static fpk_catalog_policy policy;static reader artifact,catalog;
static const uint8_t *name;static size_t name_n;
static int prepare(unsigned op){artifact.at=0;return pi_prepare(op,catalog.p,catalog.n,&policy,name,name_n,read_bytes,&artifact,&inventory,objects,&resources,&w,&plan);}
static void rejected(unsigned op){saved=plan;assert(prepare(op)&&!memcmp(&saved,&plan,sizeof plan)&&!w.verify.verified);}
long tool_syscall(long n,uintptr_t a,uintptr_t b,uintptr_t c){(void)n;(void)a;(void)b;(void)c;assert(!"unexpected codec syscall");return -1;}
int main(int argc,char **argv){
    assert(argc==5);load(argv[1],&artifact);load(argv[2],&catalog);reader bytes;load(argv[3],&bytes);assert(bytes.n==sizeof policy);memcpy(&policy,bytes.p,sizeof policy);free(bytes.p);
    assert(catalog.n>300&&catalog.p[72]>=1&&catalog.p[72]<=32);name=catalog.p+73;name_n=catalog.p[72];
    fpk_span names[]={{(const uint8_t *)"shell",5},{(const uint8_t *)"sudo",4},{(const uint8_t *)"pkg",3},{(const uint8_t *)"echo",4}};
    resources=(pi_resources){.block_size=4096,.journal_images=64,.orphan_slots=16,.free={65536,4096},.capacity={65536,4096},.memory_bytes=2*1024*1024,.base=2,.base_names=names,.base_count=4};for(unsigned i=0;i<32;i++)resources.platform[i]=i;
    inventory.sequence=inventory.generation=1;inventory.inventory_bytes=512;memset(inventory.hash,1,32);assert(!prepare(PI_INSTALL));assert(plan.operation==PI_INSTALL&&plan.file_count>=1&&plan.file_count<=16);
    assert(plan.total[0]&&plan.total[1]&&plan.memory_bytes&&plan.journal_images==64&&plan.orphan_slots==16);assert(!pi_encode(&plan,w.wire));FILE *f=fopen(argv[4],"wbx");assert(f&&fwrite(w.wire,1,8192,f)==8192&&!fclose(f));
    pi_plan installed=plan;
    for(unsigned bs=1024;bs<=2048;bs*=2){resources.block_size=bs;assert(!prepare(PI_INSTALL)&&plan.total[0]>installed.total[0]);}resources.block_size=4096;
    for(unsigned dimension=0;dimension<2;dimension++){
        uint64_t old=resources.free[dimension],reserve=dimension?16:64;
        resources.free[dimension]=reserve+installed.total[dimension]-1;rejected(PI_INSTALL);
        resources.free[dimension]++;assert(!prepare(PI_INSTALL));resources.remaining[dimension]=1;rejected(PI_INSTALL);resources.remaining[dimension]=0;resources.free[dimension]=old;
    }
    uint64_t memory=resources.memory_bytes;resources.memory_bytes=installed.memory_bytes-1;rejected(PI_INSTALL);resources.memory_bytes=memory;
    resources.journal_images=63;rejected(PI_INSTALL);resources.journal_images=64;resources.orphan_slots=15;rejected(PI_INSTALL);resources.orphan_slots=16;
    resources.platform[0]^=1;rejected(PI_INSTALL);resources.platform[0]^=1;resources.base=99;rejected(PI_INSTALL);resources.base=2;
    names[3]=(fpk_span){installed.command,installed.command_length};rejected(PI_INSTALL);names[3]=(fpk_span){(const uint8_t *)"echo",4};
    uint64_t now=policy.now;policy.now=0;rejected(PI_INSTALL);policy.now=UINT64_C(1793000000);rejected(PI_INSTALL);policy.now=now;
    inventory.sequence=UINT64_MAX;rejected(PI_INSTALL);inventory.sequence=1;inventory.generation=UINT64_MAX;rejected(PI_INSTALL);inventory.generation=1;
    resources.capacity[0]--;rejected(PI_INSTALL);resources.capacity[0]++;resources.capacity[1]--;rejected(PI_INSTALL);resources.capacity[1]++;
    resources.remaining[0]=resources.free[0]+1;rejected(PI_INSTALL);resources.remaining[0]=0;
    inventory.package_length=installed.package_length;inventory.command_length=installed.command_length;memcpy(inventory.package,installed.package,32);memcpy(inventory.command,installed.command,32);
    inventory.build_floor=installed.build+1;memset(inventory.floor_artifact,7,32);rejected(PI_INSTALL);inventory.build_floor=installed.build;rejected(PI_INSTALL);memcpy(inventory.floor_artifact,installed.artifact,32);assert(!prepare(PI_INSTALL));
    inventory.package[0]^=1;rejected(PI_INSTALL);inventory.package[0]^=1;inventory.command[0]^=1;rejected(PI_INSTALL);inventory.command[0]^=1;
    inventory.installed=1;inventory.build=installed.build;inventory.file_count=installed.file_count;memcpy(inventory.artifact,installed.artifact,32);
    for(unsigned i=0;i<inventory.file_count;i++){
        pi_file *file=installed.files+i;objects[i]=(pi_object){.inode=100+i,.generation=10+i,.mode=file->mode,.links=1,.type=1,.size=file->size,.blocks=file->blocks};memcpy(objects[i].hash,file->hash,32);inventory.owned[i]=objects[i];
    }
    if(inventory.file_count>1){pi_object original=objects[1];pi_object owned=inventory.owned[1];objects[1].inode=objects[0].inode;inventory.owned[1].inode=objects[0].inode;rejected(PI_REMOVE);objects[1]=original;inventory.owned[1]=owned;}
    assert(!prepare(PI_INSTALL)&&plan.operation==PI_NOOP&&!plan.total[0]&&!plan.total[1]&&plan.sequence==inventory.sequence&&plan.generation==inventory.generation);assert(!prepare(PI_REMOVE)&&plan.operation==PI_REMOVE);
    for(unsigned i=0;i<inventory.file_count;i++){
        pi_object original=objects[i];objects[i].hash[0]^=1;rejected(PI_REMOVE);objects[i]=original;objects[i].generation++;rejected(PI_REMOVE);objects[i]=original;objects[i].inode++;rejected(PI_REMOVE);objects[i]=original;
        objects[i].uid=1000;rejected(PI_REMOVE);objects[i]=original;objects[i].links=2;rejected(PI_REMOVE);objects[i]=original;objects[i].mode^=1;rejected(PI_REMOVE);objects[i]=original;
        objects[i].size++;rejected(PI_REMOVE);objects[i]=original;objects[i].blocks++;rejected(PI_REMOVE);objects[i]=original;objects[i].type=2;rejected(PI_REMOVE);objects[i]=original;
        objects[i].pins=2;assert(!prepare(PI_REMOVE));objects[i]=original; /* execution must defer reclamation */
    }
    uint8_t value=artifact.p[artifact.n-1];artifact.p[artifact.n-1]^=1;rejected(PI_REMOVE);artifact.p[artifact.n-1]=value;catalog.p[catalog.n-1]^=1;rejected(PI_REMOVE);catalog.p[catalog.n-1]^=1;
    saved=plan;artifact.at=0;assert(pi_prepare(PI_REMOVE,catalog.p,catalog.n,&policy,name,name_n,read_bytes,&artifact,&inventory,objects,&resources,&w,(pi_plan *)&w));assert(!memcmp(&saved,&plan,sizeof plan));
    printf("PASS verified install/remove/no-op plans, %u files, exact resource boundaries, conflicts, floors, identities and foreign-file refusal\n",installed.file_count);
    free(artifact.p);free(catalog.p);return 0;
}
