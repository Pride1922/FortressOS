#include "base.h"
#include "crypto.h"
#include "string.h"
static uint64_t get(const uint8_t *p,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
static bool zero(const uint8_t *p,size_t n){for(size_t i=0;i<n;i++)if(p[i])return false;return true;}
static int octal(const uint8_t *p,unsigned n,uint64_t *v){
    *v=0;bool digits=false;unsigned i=0;for(;i<n&&p[i]==' ';i++);
    for(;i<n&&p[i]>='0'&&p[i]<='7';i++){if(*v>(UINT64_MAX-7)/8)return -1;*v=*v*8+p[i]-'0';digits=true;}
    for(;i<n&&(p[i]==0||p[i]==' ');i++);
    return !digits||i!=n?-1:0;
}
int base_archive_release(const uint8_t *p,size_t n,uint64_t *build,uint64_t *abi,uint8_t platform[32]){
    if(!p||n<1024||n>BASE_BANK_BYTES||n%512)return -1;
    unsigned found=0,entries=0;size_t at=0;
    while(n-at>=512){
        const uint8_t *h=p+at;if(zero(h,512))return found==1&&n-at>=1024&&zero(h,n-at)?0:-1;
        uint64_t bytes,sum,uid,gid;
        if(++entries>510||memcmp(h+257,"ustar\0",6)||memcmp(h+263,"00",2)||
           octal(h+124,12,&bytes)||octal(h+148,8,&sum)||octal(h+108,8,&uid)||uid||octal(h+116,8,&gid)||gid||
           (h[156]!='0'&&h[156]!=0&&h[156]!='5')||bytes>n-at-512||bytes>BASE_BANK_BYTES)return -1;
        uint64_t check=0;for(unsigned i=0;i<512;i++)check+=i>=148&&i<156?' ':h[i];if(check!=sum)return -1;
        size_t padded=((size_t)bytes+511)&~(size_t)511;if(padded>n-at-512)return -1;
        if(!memcmp(h,"base/release.v1",16)&&zero(h+16,84)&&zero(h+345,155)){
            if(h[156]=='5'||base_release_parse(h+512,(size_t)bytes,build,abi,platform))return -1;
            found++;
        }
        at+=512+padded;
    }return -1;
}
/* Full ELF loadability is independently checked by the loader on boot. Reject
 * alternate architectures, wrapping/out-of-file segments and an absent entry. */
static bool elf(const uint8_t *p,size_t n){
    if(n<64||memcmp(p,"\177ELF\2\1\1",7)||get(p+18,2)!=62||get(p+52,2)!=64||get(p+54,2)!=56)return false;
    uint64_t at=get(p+32,8),count=get(p+56,2),entry=get(p+24,8);bool found=false;
    if(!count||count>128||at>n||count>(n-at)/56)return false;
    for(unsigned i=0;i<count;i++){
        const uint8_t *h=p+at+i*56;if(get(h,4)!=1)continue;
        uint64_t off=get(h+8,8),va=get(h+16,8),sz=get(h+32,8),mem=get(h+40,8);
        if(sz>mem||off>n||sz>n-off||mem>UINT64_MAX-va)return false;
        if((get(h+4,4)&1)&&entry>=va&&entry-va<mem)found=true;
    }return found;
}
static int base_bootstrap_shape(const uint8_t *p,size_t n){
    static const struct {const char *path;unsigned mode;bool directory,executable;} required[]={
        {"bin",0755,true,false},{"sbin",0755,true,false},{"lib",0755,true,false},{"base",0755,true,false},{"docs",0755,true,false},
        {"usr",0755,true,false},{"usr/share",0755,true,false},{"usr/share/licenses",0755,true,false},{"usr/share/wallpapers",0755,true,false},
        {"etc",0755,true,false},{"etc/ssl",0755,true,false},
        {"bin/login",0755,false,true},{"bin/shell",0755,false,true},{"bin/sudo",04755,false,true},
        {"bin/init",0755,false,true},{"bin/pkg",0755,false,true},{"bin/wget",0755,false,true},{"sbin/watchtower",0755,false,true},
        {"usr/share/zoneinfo.tzdb",0644,false,false},{"etc/ssl/certs/anchors.v1",0644,false,false}
    };
    uint32_t found=0;
    for(size_t at=0;n-at>=512;){
        const uint8_t *h=p+at;if(zero(h,512))break;uint64_t bytes,mode;
        if(octal(h+124,12,&bytes)||octal(h+100,8,&mode)||bytes>n-at-512)return -1;
        size_t nn=0,pn=0;while(nn<100&&h[nn])nn++;while(pn<155&&h[345+pn])pn++;
        if(nn==100||pn==155||nn+pn+1>=256)return -1;
        char path[256];size_t used=0;if(pn){memcpy(path,h+345,pn);used=pn;path[used++]='/';}memcpy(path+used,h,nn);used+=nn;
        bool dir=h[156]=='5';if(dir&&used&&path[used-1]=='/')used--;path[used]=0;
        for(unsigned i=0;i<sizeof required/sizeof required[0];i++)if(!strcmp(path,required[i].path)){
            if((found&(1u<<i))||mode!=required[i].mode||dir!=required[i].directory||
               (required[i].executable&&!elf(h+512,(size_t)bytes)))return -1;
            found|=1u<<i;
        }
        size_t padded=((size_t)bytes+511)&~(size_t)511;if(padded>n-at-512)return -1;at+=512+padded;
    }
    return found==(1u<<(sizeof required/sizeof required[0]))-1?0:-1;
}
int base_archive_command(const uint8_t *p,size_t n,fpk_span command){
    if(!p||!fpk_identity_valid(command.bytes,command.length))return -1;
    for(size_t at=0;n-at>=512;){
        const uint8_t *h=p+at;if(zero(h,512))return 0;
        uint64_t bytes;if(octal(h+124,12,&bytes)||bytes>n-at-512)return -1;
        size_t name=0,prefix=0;while(name<100&&h[name])name++;while(prefix<155&&h[345+prefix])prefix++;
        if(name==100||prefix==155||name+prefix+1>=256)return -1;
        char path[256];size_t used=0;
        if(prefix){memcpy(path,h+345,prefix);used=prefix;path[used++]='/';}
        memcpy(path+used,h,name);used+=name;path[used]=0;
        const char *starts[]={"bin/","sbin/","usr/sbin/"};
        for(unsigned i=0;i<3;i++){size_t sn=strlen(starts[i]);if(used==sn+command.length&&!memcmp(path,starts[i],sn)&&!memcmp(path+sn,command.bytes,command.length))return 1;}
        size_t padded=((size_t)bytes+511)&~(size_t)511;if(padded>n-at-512)return -1;at+=512+padded;
    }return -1;
}
int base_artifact_parse(const uint8_t *p,size_t n,base_artifact *out){
    if(!p||!out||n<BASE_HEADER_BYTES||n>BASE_ARTIFACT_MAX||memcmp(p,"FORTBAS1",8)||
       get(p+8,4)!=1||get(p+12,4)!=BASE_HEADER_BYTES||!get(p+16,8)||
       get(p+24,8)>get(p+32,8)||get(p+32,8)>=get(p+16,8)||!get(p+24,8)||zero(p+56,32)||
       get(p+152,8)!=BASE_LAYOUT||!get(p+160,8)||!zero(p+168,88))return -1;
    uint64_t kn=get(p+40,8),bn=get(p+48,8);
    if(kn<64||kn>BASE_BANK_BYTES||bn<1024||bn>BASE_BANK_BYTES-2560||bn%512||
       kn>n-BASE_HEADER_BYTES||bn!=n-BASE_HEADER_BYTES-kn||!elf(p+BASE_HEADER_BYTES,(size_t)kn))return -1;
    uint8_t digest[32];
    if(crypto_sha256(p+BASE_HEADER_BYTES,(size_t)kn,digest)||memcmp(digest,p+88,32)||
       crypto_sha256(p+BASE_HEADER_BYTES+kn,(size_t)bn,digest)||memcmp(digest,p+120,32))return -1;
    uint64_t build,abi;uint8_t platform[32];
    if(base_archive_release(p+BASE_HEADER_BYTES+kn,(size_t)bn,&build,&abi,platform)||build!=get(p+16,8)||
       abi!=get(p+160,8)||memcmp(platform,p+56,32)||base_bootstrap_shape(p+BASE_HEADER_BYTES+kn,(size_t)bn))return -1;
    *out=(base_artifact){.build=get(p+16,8),.predecessor_min=get(p+24,8),.predecessor_max=get(p+32,8),.abi=get(p+160,8),
        .platform=p+56,.kernel_hash=p+88,.bootstrap_hash=p+120,.kernel=p+BASE_HEADER_BYTES,
        .bootstrap=p+BASE_HEADER_BYTES+kn,.kernel_bytes=(size_t)kn,.bootstrap_bytes=(size_t)bn};return 0;
}
int base_artifact_verify(const uint8_t *p,size_t n,const uint8_t *wire,size_t wn,
                         const fpk_catalog_policy *policy,base_artifact *out){
    fpk_catalog catalog;fpk_catalog_target target;base_artifact value;uint8_t hash[32];
    if(fpk_catalog_verify(wire,wn,policy,&catalog)||fpk_catalog_find(&catalog,(const uint8_t *)"fortress-base",13,&target)||
       n!=target.length||crypto_sha256(p,n,hash)||memcmp(hash,target.sha256,32)||
       base_artifact_parse(p,n,&value)||value.build!=target.build)return -1;
    *out=value;return 0;
}
