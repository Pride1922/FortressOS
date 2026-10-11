#include "root_boot.h"
#include "string.h"
#include "../../user/tools/digest.h"
#include "../../user/tools/userdb.h"

root_boot_config_t root_boot_parse(const char *cmd,size_t capacity,bool truncated) {
    root_boot_config_t out={.mode=ROOT_BOOT_LIVE,.valid=true};
    if (!cmd || !capacity || truncated) {out.valid=false;return out;}
    size_t length=0;
    while (length<capacity && cmd[length]) length++;
    if (length==capacity) {out.valid=false;return out;}
    for (size_t pos=0;pos<length;) {
        while (pos<length && (cmd[pos]==' ' || cmd[pos]=='\t')) pos++;
        size_t start=pos;
        while (pos<length && cmd[pos]!=' ' && cmd[pos]!='\t') pos++;
        size_t n=pos-start;const char *token=cmd+start;
        if (n>=10 && !memcmp(token,"boot_mode=",10)) {
            if (out.explicit_mode) out.valid=false;
            out.explicit_mode=true;
            if (n==14 && !memcmp(token+10,"live",4)) out.mode=ROOT_BOOT_LIVE;
            else if (n==19 && !memcmp(token+10,"installed",9)) out.mode=ROOT_BOOT_INSTALLED;
            else if (n==18 && !memcmp(token+10,"recovery",8)) out.mode=ROOT_BOOT_RECOVERY;
            else if (n==19 && !memcmp(token+10,"installer",9)) out.mode=ROOT_BOOT_INSTALLER;
            else out.valid=false;
        } else if (n>=5 && !memcmp(token,"root=",5)) {
            if (out.has_root) out.valid=false;
            out.has_root=true;
            char guid[37];
            if (n!=50 || memcmp(token,"root=PARTUUID=",14)) {out.valid=false;continue;}
            memcpy(guid,token+14,36);guid[36]=0;
            if (!gpt_str_to_guid(guid,&out.partuuid)) out.valid=false;
            bool nonzero=false;
            const uint8_t *raw=(const void *)&out.partuuid;
            for (unsigned i=0;i<16;i++) nonzero|=raw[i]!=0;
            if (!nonzero) out.valid=false;
        }
    }
    if (out.mode==ROOT_BOOT_LIVE || out.mode==ROOT_BOOT_INSTALLER ? out.has_root : !out.has_root) out.valid=false;
    return out;
}

static bool field(const char *b,size_t n,size_t *p,const char *key,char *out,size_t capacity,size_t exact) {
    size_t k=strlen(key);
    if (*p>n || k>n-*p || memcmp(b+*p,key,k)) return false;
    *p+=k;size_t start=*p;
    while (*p<n && b[*p]!='\n') {
        if (b[*p]<' ' || b[*p]>'~') return false;
        (*p)++;
    }
    size_t len=*p-start;
    if (*p==n || len>=capacity || (exact && len!=exact)) return false;
    memcpy(out,b+start,len);out[len]=0;(*p)++;return true;
}
static bool magic(const char *b,size_t n,size_t *p,const char *text) {
    size_t l=strlen(text);if (l>n || memcmp(b,text,l)) return false;
    *p=l;return true;
}
static bool lowerhex(const char *s,size_t n) {
    for (size_t i=0;i<n;i++) if (!((s[i]>='0' && s[i]<='9') || (s[i]>='a' && s[i]<='f'))) return false;
    return true;
}
static bool identity(const char *b,size_t n,size_t *p,root_recovery_t *r) {
    gpt_guid_t guid;char canonical[37];
    if (!field(b,n,p,"install_id=",r->install_id,sizeof(r->install_id),32) || !lowerhex(r->install_id,32) ||
        !field(b,n,p,"root_partuuid=",r->partuuid,sizeof(r->partuuid),36) || !gpt_str_to_guid(r->partuuid,&guid)) return false;
    gpt_guid_to_str(&guid,canonical);
    /* GPT display is uppercase; canonical lowercase is validated character by character. */
    for (unsigned i=0;i<36;i++) if (r->partuuid[i]!='-' && !lowerhex(r->partuuid+i,1)) return false;
    bool nonzero=false;for (unsigned i=0;i<16;i++) nonzero|=guid.bytes[i]!=0;
    if (!nonzero || !field(b,n,p,"filesystem_uuid=",r->filesystem_uuid,sizeof(r->filesystem_uuid),36) ||
        !gpt_str_to_guid(r->filesystem_uuid,&guid)) return false;
    for (unsigned i=0;i<36;i++) if (r->filesystem_uuid[i]!='-' && !lowerhex(r->filesystem_uuid+i,1)) return false;
    nonzero=false;for (unsigned i=0;i<16;i++) nonzero|=guid.bytes[i]!=0;
    return nonzero;
}
bool root_recovery_parse(const void *bytes,size_t n,root_recovery_t *out) {
    if (!bytes || !out || n>512) return false;
    const char *b=bytes;size_t p=0;root_recovery_t r={0};
    if (!magic(b,n,&p,"FORTRESS-RECOVERY-1\n") || !identity(b,n,&p,&r) ||
        !field(b,n,&p,"password_hash=",r.hash,sizeof(r.hash),0) || p!=n ||
        strncmp(r.hash,"$5$rounds=100000$",17) || strlen(r.hash)!=77 || r.hash[33]!='$' || !db_hash_valid(r.hash)) return false;
    *out=r;return true;
}
static bool same_identity(const root_recovery_t *a,const root_recovery_t *b) {
    return !strcmp(a->install_id,b->install_id) && !strcmp(a->partuuid,b->partuuid) &&
        !strcmp(a->filesystem_uuid,b->filesystem_uuid);
}
static bool hash_matches(const void *bytes,size_t n,const char *hex) {
    digest_t d;uint8_t raw[32];char text[65];static const char alphabet[]="0123456789abcdef";
    digest_init(&d,DIGEST_SHA256);if (!digest_update(&d,bytes,n)) return false;
    digest_final(&d,raw);
    for (unsigned i=0;i<32;i++) {text[i*2]=alphabet[raw[i]>>4];text[i*2+1]=alphabet[raw[i]&15];}
    text[64]=0;return !strcmp(text,hex);
}
bool root_manifest_validate(const void *bytes,size_t n,const root_recovery_t *r) {
    if (!bytes || !r || n>512) return false;
    size_t p=0;root_recovery_t next={0};char version[2];
    return magic(bytes,n,&p,"FORTRESS-INSTALL-1\n") && identity(bytes,n,&p,&next) &&
        field(bytes,n,&p,"payload_version=",version,sizeof(version),1) && version[0]=='1' && p==n && same_identity(r,&next);
}
bool root_readiness_validate(const void *bytes,size_t n,const root_recovery_t *r,
                             const void *kernel,size_t kn,const void *bootstrap,size_t bn) {
    if (!bytes || n!=512 || !r || !kernel || !kn || kn>32u*1024*1024 || !bootstrap || !bn || bn>8u*1024*1024) return false;
    size_t p=0;root_recovery_t next={0};char version[2],kh[65],bh[65],checksum[65];
    if (!magic(bytes,n,&p,"FORTRESS-READY-1\n") || !identity(bytes,n,&p,&next) || !same_identity(r,&next) ||
        !field(bytes,n,&p,"payload_version=",version,sizeof(version),1) || version[0]!='1' ||
        !field(bytes,n,&p,"kernel_sha256=",kh,sizeof(kh),64) || !lowerhex(kh,64) ||
        !field(bytes,n,&p,"bootstrap_sha256=",bh,sizeof(bh),64) || !lowerhex(bh,64)) return false;
    size_t covered=p;
    if (!field(bytes,n,&p,"checksum=",checksum,sizeof(checksum),64) || !lowerhex(checksum,64)) return false;
    for (size_t i=p;i<n;i++) if (((const char *)bytes)[i]) return false;
    return hash_matches(bytes,covered,checksum) && hash_matches(kernel,kn,kh) && hash_matches(bootstrap,bn,bh);
}
