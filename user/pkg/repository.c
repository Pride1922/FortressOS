#include "repository.h"
#include "string.h"
static bool repo_apart(const void *a,size_t an,const void *b,size_t bn){
    return a&&b&&an<=UINTPTR_MAX-(uintptr_t)a&&bn<=UINTPTR_MAX-(uintptr_t)b&&((uintptr_t)a+an<=(uintptr_t)b||(uintptr_t)b+bn<=(uintptr_t)a);
}
int pkg_repository_decode(const uint8_t *p,size_t n,pkg_repository *out){
    static const char prefix[]="version=1\nurl=";
    if(!repo_apart(out,sizeof *out,p,n)||n<sizeof prefix+8||n>512||memcmp(p,prefix,sizeof prefix-1)||p[n-1]!='\n')return -1;
    p+=sizeof prefix-1;n-=sizeof prefix; /* omit the final newline */
    if(!n||n>=PKG_REPO_BASE_MAX)return -1;
    size_t scheme=n>=8&&!memcmp(p,"https://",8)?8:n>=7&&!memcmp(p,"http://",7)?7:0;
    if(!scheme)return -1;
    size_t at=scheme,label=0,host=0;
    for(;at<n&&p[at]!=':'&&p[at]!='/';at++){
        uint8_t c=p[at];host++;
        if(c=='.'){if(!label||p[at-1]=='-')return -1;label=0;continue;}
        if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||(c=='-'&&label)))return -1;
        if(++label>63)return -1;
    }
    if(!host||host>253||!label||p[at-1]=='-')return -1;
    if(at<n&&p[at]==':'){
        at++;size_t start=at;unsigned port=0;
        while(at<n&&p[at]!='/'){
            if(p[at]<'0'||p[at]>'9'||port>6553)return -1;
            port=port*10+(p[at++]-'0');if(port>65535)return -1;
        }
        if(at==start||!port||(at-start>1&&p[start]=='0'))return -1;
    }
    while(at<n){
        if(p[at++]!='/')return -1;
        size_t start=at;
        while(at<n&&p[at]!='/'){
            uint8_t c=p[at++];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'))return -1;
        }
        size_t length=at-start;
        if(!length||(length==1&&p[start]=='.')||(length==2&&p[start]=='.'&&p[start+1]=='.'))return -1;
    }
    memset(out,0,sizeof *out);memcpy(out->base,p,n);return 0;
}
int pkg_repository_url(const pkg_repository *r,const uint8_t *hash,char out[PKG_REPO_URL_MAX]){
    if(!repo_apart(out,PKG_REPO_URL_MAX,r,sizeof *r)||(hash&&!repo_apart(out,PKG_REPO_URL_MAX,hash,32)))return -1;
    size_t n=0;while(n<sizeof r->base&&r->base[n])n++;
    if(!n||n==sizeof r->base)return -1;
    const char *suffix=hash?"/objects/sha256/":"/catalog.v1";size_t length=strlen(suffix);
    if(n+length+(hash?68:0)>=PKG_REPO_URL_MAX)return -1;
    memcpy(out,r->base,n);memcpy(out+n,suffix,length);n+=length;
    if(hash){static const char digits[]="0123456789abcdef";for(unsigned i=0;i<32;i++){out[n++]=digits[hash[i]>>4];out[n++]=digits[hash[i]&15];}memcpy(out+n,".fpk",4);n+=4;}
    out[n]=0;return 0;
}
static int hex_digit(uint8_t c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;}
static bool trust_hex(const uint8_t *p,size_t n,uint8_t *out){
    bool nonzero=false;
    for(size_t i=0;i<n;i++){
        int high=hex_digit(p[2*i]),low=hex_digit(p[2*i+1]);if(high<0||low<0)return false;
        out[i]=(uint8_t)(high*16+low);nonzero|=out[i]!=0;
    }
    return nonzero;
}
static bool trust_field(const uint8_t *p,size_t n,size_t *at,const char *name,const uint8_t **value,size_t *length){
    size_t prefix=strlen(name);if(*at>n||prefix>n-*at||memcmp(p+*at,name,prefix))return false;
    *at+=prefix;size_t start=*at;while(*at<n&&p[*at]!='\n')(*at)++;
    if(*at==n||*at==start)return false;
    *value=p+start;*length=*at-start;(*at)++;return true;
}
static bool trust_number(const uint8_t *p,size_t n,uint64_t *out){
    if(!n||n>20||p[0]=='0')return false;
    uint64_t value=0;
    for(size_t i=0;i<n;i++){
        if(p[i]<'0'||p[i]>'9'||value>(UINT64_MAX-(p[i]-'0'))/10)return false;
        value=value*10+(p[i]-'0');
    }
    if(value==UINT64_MAX)return false;
    *out=value;return true;
}
int pkg_repository_trust_decode(const uint8_t *p,size_t n,uint64_t now,pkg_repository_trust *out){
    if(n>1024||!repo_apart(out,sizeof *out,p,n)||now<1767225600ULL||now>4102444800ULL)return -1;
    pkg_repository_trust trust={0};size_t at=0,length;const uint8_t *value;
    if(!trust_field(p,n,&at,"version=",&value,&length)||length!=1||value[0]!='1'||
       !trust_field(p,n,&at,"repository=",&value,&length)||length!=36)return -1;
    uint8_t uuid[32];size_t count=0;
    for(size_t i=0;i<36;i++){
        if(i==8||i==13||i==18||i==23){if(value[i]!='-')return -1;}
        else uuid[count++]=value[i];
    }
    if(!trust_hex(uuid,16,trust.catalog.repository)||
       !trust_field(p,n,&at,"root_version=",&value,&length)||!trust_number(value,length,&trust.catalog.root_version)||
       !trust_field(p,n,&at,"platform=",&value,&length)||length!=64||!trust_hex(value,32,trust.platform)||
       !trust_field(p,n,&at,"base_version=",&value,&length)||!trust_number(value,length,&trust.base_version))return -1;
    while(at<n){
        unsigned i=trust.catalog.key_count;if(i==8||!trust_field(p,n,&at,"public_key=",&value,&length)||length!=64||
            !trust_hex(value,32,trust.catalog.keys[i]))return -1;
        for(unsigned j=0;j<i;j++)if(!memcmp(trust.catalog.keys[j],trust.catalog.keys[i],32))return -1;
        trust.catalog.key_count++;
    }
    if(!trust.catalog.key_count)return -1;
    trust.catalog.threshold=1;trust.catalog.now=now;*out=trust;return 0;
}
