/* Bounded OpenSSH container adapter. See docs/plans/SSH_PHASE3_HOST_IDENTITY.md.
 * No SSH transport, disk policy or key publication is performed here. */
#include "internal.h"
#include "ssh_hostkey.h"

static const char begin[]="-----BEGIN OPENSSH PRIVATE KEY-----";
static const char end[]="-----END OPENSSH PRIVATE KEY-----";
static const char algorithm[]="ssh-ed25519";
static const char comment[]="FortressOS host key";
static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
typedef struct { const uint8_t *p; size_t n; } reader;

static void put32(uint8_t *p,uint32_t v) {
    p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v;
}
static size_t string_put(uint8_t *p,const void *v,size_t n) {
    put32(p,(uint32_t)n);if(n) memcpy(p+4,v,n);return n+4;
}
static bool number(reader *r,uint32_t *v) {
    if(r->n<4) return false;
    *v=(uint32_t)r->p[0]<<24 | (uint32_t)r->p[1]<<16 | (uint32_t)r->p[2]<<8 | r->p[3];
    r->p+=4;r->n-=4;return true;
}
static bool string_get(reader *r,reader *v) {
    uint32_t n;if(!number(r,&n) || n>r->n) return false;
    *v=(reader){r->p,n};r->p+=n;r->n-=n;return true;
}
static bool literal(reader *r,const void *p,size_t n) {
    reader v;return string_get(r,&v) && v.n==n && (!n || !memcmp(v.p,p,n));
}
static size_t blob(uint8_t *p,const uint8_t public_key[32]) {
    size_t n=string_put(p,algorithm,sizeof(algorithm)-1);
    return n+string_put(p+n,public_key,32);
}
static size_t base64(char *out,const uint8_t *p,size_t n,bool pad) {
    size_t at=0;
    for(size_t i=0;i<n;i+=3) {
        uint32_t v=(uint32_t)p[i]<<16;
        size_t left=n-i;if(left>1) v|=(uint32_t)p[i+1]<<8;if(left>2) v|=p[i+2];
        out[at++]=alphabet[v>>18];out[at++]=alphabet[(v>>12)&63];
        if(left>1) out[at++]=alphabet[(v>>6)&63];else if(pad) out[at++]='=';
        if(left>2) out[at++]=alphabet[v&63];else if(pad) out[at++]='=';
    }
    return at;
}
static int digit(unsigned c) {
    if(c>='A' && c<='Z') return (int)c-'A';
    if(c>='a' && c<='z') return (int)c-'a'+26;
    if(c>='0' && c<='9') return (int)c-'0'+52;
    return c=='+' ? 62 : c=='/' ? 63 : -1;
}
/* All six ranges are checked before workspace use. Even rejected aliases must
 * not be wiped: wiping an aliased workspace would publish into failed output. */
static bool ranges(ssh_hostkey_workspace *w,const void *in,size_t n,void *out,size_t cap,size_t *len) {
    if(!cr_range(w,sizeof(*w)) || !cr_range(in,n) || !cr_range(out,cap) ||
       (len && !cr_range(len,sizeof(*len)))) return false;
    return !cr_overlap(w,sizeof(*w),in,n) && !cr_overlap(w,sizeof(*w),out,cap) &&
        !cr_overlap(in,n,out,cap) && (!len ||
        (!cr_overlap(w,sizeof(*w),len,sizeof(*len)) && !cr_overlap(in,n,len,sizeof(*len)) &&
         !cr_overlap(out,cap,len,sizeof(*len))));
}
static int checked_secret(ssh_hostkey_workspace *w,const uint8_t sk[64]) {
    int r=crypto_ed25519_from_seed(sk,w->public_key,w->secret_key);
    if(!r && crypto_verify32(sk+32,w->public_key)) r=SYSCALL_EINVAL;
    return r;
}
static int encode(ssh_hostkey_workspace *w,uint32_t check,char *out,size_t cap,size_t *len) {
    uint8_t *p=w->binary;size_t n=0;
    memcpy(p,"openssh-key-v1",15);n=15;
    n+=string_put(p+n,"none",4);n+=string_put(p+n,"none",4);n+=string_put(p+n,NULL,0);
    put32(p+n,1);n+=4;put32(p+n,51);n+=4;n+=blob(p+n,w->public_key);
    size_t mark=n;n+=4;size_t start=n;
    put32(p+n,check);put32(p+n+4,check);n+=8;
    n+=string_put(p+n,algorithm,sizeof(algorithm)-1);
    n+=string_put(p+n,w->public_key,32);n+=string_put(p+n,w->secret_key,64);
    n+=string_put(p+n,comment,sizeof(comment)-1);
    /* OpenSSH adds zero padding bytes when the section is already aligned. */
    for(unsigned v=1;(n-start)%8;v++) p[n++]=(uint8_t)v;
    put32(p+mark,(uint32_t)(n-start));
    size_t b64=base64(w->text,p,n,true);
    size_t needed=sizeof(begin)-1+1+b64+(b64+69)/70+sizeof(end)-1+1;
    if(cap<=needed) return SYSCALL_EINVAL;
    /* No failures beyond this point; publish only a fully formed container. */
    size_t at=0;memcpy(out,begin,sizeof(begin)-1);at+=sizeof(begin)-1;out[at++]='\n';
    for(size_t i=0;i<b64;i+=70) {
        size_t k=b64-i;if(k>70) k=70;
        memcpy(out+at,w->text+i,k);at+=k;out[at++]='\n';
    }
    memcpy(out+at,end,sizeof(end)-1);at+=sizeof(end)-1;out[at++]='\n';out[at]=0;*len=at;
    return 0;
}
int ssh_hostkey_encode(const uint8_t sk[64],uint32_t check,char *out,size_t cap,size_t *len,ssh_hostkey_workspace *w) {
    if(!len || !ranges(w,sk,64,out,cap,len)) return SYSCALL_EINVAL;
    int r=checked_secret(w,sk);if(!r) r=encode(w,check,out,cap,len);
    crypto_wipe(w,sizeof(*w));return r;
}
static bool line(reader *r,reader *v) {
    size_t n=0;while(n<r->n && r->p[n]!='\n') n++;
    if(n==r->n) return false;
    *v=(reader){r->p,n};r->p+=n+1;r->n-=n+1;
    if(v->n && v->p[v->n-1]=='\r') v->n--;
    return true;
}
static int unarmor(ssh_hostkey_workspace *w,const char *text,size_t n,size_t *decoded) {
    reader r={(const uint8_t *)text,n},v;
    if(!line(&r,&v) || v.n!=sizeof(begin)-1 || memcmp(v.p,begin,v.n)) return SYSCALL_EINVAL;
    size_t b=0;bool found=false;
    while(line(&r,&v)) {
        if(v.n==sizeof(end)-1 && !memcmp(v.p,end,v.n)) { found=true;break; }
        if(!v.n || v.n>70 || v.n>sizeof(w->text)-b) return SYSCALL_EINVAL;
        for(size_t i=0;i<v.n;i++) if(digit(v.p[i])<0 && v.p[i]!='=') return SYSCALL_EINVAL;
        memcpy(w->text+b,v.p,v.n);b+=v.n;
    }
    if(!found || r.n || !b || b%4) return SYSCALL_EINVAL;
    size_t at=0;
    for(size_t i=0;i<b;i+=4) {
        int a=digit((unsigned char)w->text[i]),c=digit((unsigned char)w->text[i+1]);
        int d=digit((unsigned char)w->text[i+2]),e=digit((unsigned char)w->text[i+3]);
        bool pad2=w->text[i+2]=='=',pad1=w->text[i+3]=='=';
        if(a<0 || c<0 || (!pad2 && d<0) || (!pad1 && e<0) ||
           (pad2 && !pad1) || ((pad1 || pad2) && i+4!=b) ||
           (pad2 && (c&15)) || (pad1 && !pad2 && (d&3)) || at+3>sizeof(w->binary)) return SYSCALL_EINVAL;
        uint32_t x=(uint32_t)a<<18 | (uint32_t)c<<12 | (uint32_t)(d<0 ? 0 : d)<<6 | (uint32_t)(e<0 ? 0 : e);
        w->binary[at++]=(uint8_t)(x>>16);
        if(!pad2) w->binary[at++]=(uint8_t)(x>>8);
        if(!pad1) w->binary[at++]=(uint8_t)x;
    }
    *decoded=at;return 0;
}
static int decode(ssh_hostkey_workspace *w,const char *text,size_t n) {
    size_t bytes;int error=unarmor(w,text,n,&bytes);if(error) return error;
    reader r={w->binary,bytes},pub,priv,v;uint32_t count,a,b;
    if(r.n<15 || memcmp(r.p,"openssh-key-v1",15)) return SYSCALL_EINVAL;
    r.p+=15;r.n-=15;
    if(!literal(&r,"none",4) || !literal(&r,"none",4) || !literal(&r,NULL,0) ||
       !number(&r,&count) || count!=1 || !string_get(&r,&pub) ||
       !string_get(&r,&priv) || r.n || !priv.n || priv.n%8) return SYSCALL_EINVAL;
    if(!literal(&pub,algorithm,sizeof(algorithm)-1) || !string_get(&pub,&v) || v.n!=32 || pub.n) return SYSCALL_EINVAL;
    memcpy(w->public_key,v.p,32);
    if(!number(&priv,&a) || !number(&priv,&b) || a!=b ||
       !literal(&priv,algorithm,sizeof(algorithm)-1) || !string_get(&priv,&v) || v.n!=32 ||
       crypto_verify32(v.p,w->public_key) || !string_get(&priv,&v) || v.n!=64 ||
       crypto_verify32(v.p+32,w->public_key)) return SYSCALL_EINVAL;
    memcpy(w->secret_key,v.p,64);
    if(!string_get(&priv,&v) || v.n>64 || priv.n>7) return SYSCALL_EINVAL;
    for(size_t i=0;i<v.n;i++) if(v.p[i]<32 || v.p[i]>126) return SYSCALL_EINVAL;
    for(size_t i=0;i<priv.n;i++) if(priv.p[i]!=i+1) return SYSCALL_EINVAL;
    /* Regenerate into independent storage; checking repeated public halves is
     * insufficient to detect a forged seed. Scratch is wiped by the caller. */
    uint8_t pk[32],sk[64];
    error=crypto_ed25519_from_seed(w->secret_key,pk,sk);
    if(!error && crypto_verify32(pk,w->public_key)) error=SYSCALL_EINVAL;
    crypto_wipe(pk,sizeof(pk));crypto_wipe(sk,sizeof(sk));return error;
}
int ssh_hostkey_decode(const char *text,size_t n,uint8_t sk[64],ssh_hostkey_workspace *w) {
    if(!ranges(w,text,n,sk,64,NULL)) return SYSCALL_EINVAL;
    int r=n>SSH_HOSTKEY_MAX ? SYSCALL_EINVAL : decode(w,text,n);
    if(!r) memcpy(sk,w->secret_key,64);
    crypto_wipe(w,sizeof(*w));return r;
}
int ssh_hostkey_generate(char *out,size_t cap,size_t *len,ssh_hostkey_workspace *w) {
    if(!len || !ranges(w,NULL,0,out,cap,len)) return SYSCALL_EINVAL;
    uint8_t random[32]={0};
    int r=crypto_ed25519_keygen(w->public_key,w->secret_key);
    if(!r) r=cr_entropy(random);
    static const uint8_t message[]="FortressOS host key self-check v1";
    if(!r) r=crypto_ed25519_sign(w->signature,w->secret_key,message,sizeof(message)-1);
    if(!r) r=crypto_ed25519_verify(w->signature,w->public_key,message,sizeof(message)-1);
    if(!r) {
        uint32_t check=(uint32_t)random[0]<<24 | (uint32_t)random[1]<<16 | (uint32_t)random[2]<<8 | random[3];
        r=encode(w,check,out,cap,len);
    }
    crypto_wipe(random,sizeof(random));crypto_wipe(w,sizeof(*w));return r;
}
static int exported(const uint8_t pk[32],char *out,size_t cap,size_t *len,ssh_hostkey_workspace *w,bool fingerprint) {
    if(!len || !ranges(w,pk,32,out,cap,len)) return SYSCALL_EINVAL;
    size_t n=blob(w->binary,pk),at=0;int r=0;
    if(fingerprint) {
        r=crypto_sha256(w->binary,n,w->public_key);
        if(!r) {memcpy(w->text,"SHA256:",7);at=7;at+=base64(w->text+at,w->public_key,32,false);}
    } else {
        memcpy(w->text,algorithm,sizeof(algorithm)-1);at=sizeof(algorithm)-1;w->text[at++]=' ';
        at+=base64(w->text+at,w->binary,n,true);w->text[at++]=' ';
        memcpy(w->text+at,comment,sizeof(comment)-1);at+=sizeof(comment)-1;w->text[at++]='\n';
    }
    if(!r && cap<=at) r=SYSCALL_EINVAL;
    if(!r) {w->text[at]=0;memcpy(out,w->text,at+1);*len=at;}
    crypto_wipe(w,sizeof(*w));return r;
}
int ssh_hostkey_public(const uint8_t pk[32],char *out,size_t cap,size_t *len,ssh_hostkey_workspace *w) {
    return exported(pk,out,cap,len,w,false);
}
int ssh_hostkey_fingerprint(const uint8_t pk[32],char *out,size_t cap,size_t *len,ssh_hostkey_workspace *w) {
    return exported(pk,out,cap,len,w,true);
}
