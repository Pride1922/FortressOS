#include "userdb.h"
#include "digest.h"

static size_t length(const char *s) {size_t n=0;while (s[n]) n++;return n;}
static bool equal(const char *a,const char *b) {while (*a && *a==*b) {a++;b++;}return *a==*b;}
static bool copy(char *to,size_t cap,const char *from) {
    size_t n=length(from);if (n>=cap) return false;
    for (size_t i=0;i<=n;i++) to[i]=from[i];
    return true;
}
static bool name(const char *s) {
    size_t n=length(s);if (!n || n>=DB_NAME_MAX) return false;
    for (size_t i=0;i<n;i++) if (!((s[i]>='a' && s[i]<='z') || (s[i]>='A' && s[i]<='Z') ||
        (s[i]>='0' && s[i]<='9') || s[i]=='_' || s[i]=='-')) return false;
    return true;
}
static bool decimal(const char *s,uint32_t *out) {
    uint32_t n=0;if (!*s) return false;
    for (;*s;s++) {if (*s<'0' || *s>'9' || n>(UINT32_MAX-(unsigned)(*s-'0'))/10) return false;n=n*10+(*s-'0');}
    *out=n;return true;
}
static bool path(const char *s) {
    if (*s!='/' || length(s)>=128) return false;
    /* Database paths are absolute canonical paths: no dot navigation. */
    const char *p=s+1;while (*p) {const char *start=p;while (*p && *p!='/') p++;
        size_t n=p-start;if (!n || (n==1 && start[0]=='.') || (n==2 && start[0]=='.' && start[1]=='.')) return false;
        if (*p) {p++;if (!*p) return false;}}
    return true;
}
static bool members(const char *s) {
    if (!*s) return true;
    char item[32];size_t n=0;
    for (;;) {if (*s==',' || !*s) {item[n]=0;if (!name(item)) return false;n=0;if (!*s) return true;}
        else {if (n>=31) return false;item[n++]=*s;}s++;}
}
/* Parse the entire file before publishing even when the sought name appears
 * early. Embedded NUL, controls, extra/missing fields and oversize reject. */
static bool parse(userdb_t *out,const char *bytes,size_t size,unsigned kind) {
    if (!out || !bytes || !size || size>DB_FILE_MAX) return false;
    static userdb_t next;
    next=*out;unsigned count=0;size_t off=0;
    while (off<size) {
        char line[DB_LINE_MAX+1];size_t n=0;
        while (off<size && bytes[off]!='\n') {
            unsigned char c=bytes[off++];if (c<32 || c>126 || n==DB_LINE_MAX) return false;line[n++]=(char)c;
        }
        if (off<size) off++;
        line[n]=0;
        if (!n || line[0]=='#') continue;
        if (count==DB_MAX_RECORDS) return false;
        char *fields[9]={line};unsigned nf=1;
        for (size_t i=0;i<n;i++) if (line[i]==':') {line[i]=0;if (nf==9) return false;fields[nf++]=line+i+1;}
        if (nf!=(kind==0 ? 7u : kind==1 ? 4u : 9u) || !name(fields[0])) return false;
        if (kind==0) {
            db_user_t u={0};
            if (!equal(fields[1],"x") || !decimal(fields[2],&u.uid) || !decimal(fields[3],&u.gid) ||
                !path(fields[5]) || !path(fields[6]) || !copy(u.name,sizeof(u.name),fields[0]) ||
                !copy(u.home,sizeof(u.home),fields[5]) || !copy(u.shell,sizeof(u.shell),fields[6])) return false;
            for (unsigned i=0;i<count;i++) if (equal(next.users[i].name,u.name) || next.users[i].uid==u.uid) return false;
            next.users[count]=u;
        } else if (kind==1) {
            db_group_t g={0};
            if (!(equal(fields[1],"x") || !*fields[1]) || !decimal(fields[2],&g.gid) || !members(fields[3]) ||
                !copy(g.name,sizeof(g.name),fields[0]) || !copy(g.members,sizeof(g.members),fields[3])) return false;
            for (unsigned i=0;i<count;i++) if (equal(next.groups[i].name,g.name) || next.groups[i].gid==g.gid) return false;
            next.groups[count]=g;
        } else {
            db_shadow_t s={0};
            if (!copy(s.name,sizeof(s.name),fields[0]) || !copy(s.hash,sizeof(s.hash),fields[1]) ||
                !db_hash_valid(s.hash)) return false;
            for (unsigned i=2;i<9;i++) {uint32_t ignored;
                if (*fields[i] && !equal(fields[i],"-1") && !decimal(fields[i],&ignored)) return false;}
            /* No calendar clock: accounts requiring expiration/aging enforce
             * fail-closed rather than silently bypass these restrictions. */
            s.restricted=equal(fields[2],"0") || (*fields[4] && !equal(fields[4],"-1")) ||
                (*fields[6] && !equal(fields[6],"-1")) || (*fields[7] && !equal(fields[7],"-1")) || *fields[8];
            for (unsigned i=0;i<count;i++) if (equal(next.shadows[i].name,s.name)) return false;
            next.shadows[count]=s;
        }
        count++;
    }
    if (!count) return false;
    if (kind==0) next.nusers=count;else if (kind==1) next.ngroups=count;else next.nshadows=count;
    *out=next;db_wipe(&next,sizeof(next));return true;
}
bool db_passwd(userdb_t *d,const char *s,size_t n) {return parse(d,s,n,0);}
bool db_group(userdb_t *d,const char *s,size_t n) {return parse(d,s,n,1);}
bool db_shadow(userdb_t *d,const char *s,size_t n) {return parse(d,s,n,2);}
const db_user_t *db_user_name(const userdb_t *d,const char *s) {for (unsigned i=0;i<d->nusers;i++) if (equal(d->users[i].name,s)) return &d->users[i];return 0;}
const db_user_t *db_user_id(const userdb_t *d,uint32_t id) {for (unsigned i=0;i<d->nusers;i++) if (d->users[i].uid==id) return &d->users[i];return 0;}
const db_group_t *db_group_id(const userdb_t *d,uint32_t id) {for (unsigned i=0;i<d->ngroups;i++) if (d->groups[i].gid==id) return &d->groups[i];return 0;}
const db_shadow_t *db_shadow_name(const userdb_t *d,const char *s) {for (unsigned i=0;i<d->nshadows;i++) if (equal(d->shadows[i].name,s)) return &d->shadows[i];return 0;}
static bool member(const char *list,const char *who) {
    while (*list) {const char *start=list;while (*list && *list!=',') list++;
        size_t n=list-start;if (n==length(who)) {size_t i=0;while (i<n && start[i]==who[i]) i++;if (i==n) return true;}
        if (*list) list++;}return false;
}
bool db_groups(const userdb_t *d,const db_user_t *u,uint32_t out[16],unsigned *count) {
    unsigned n=1;out[0]=u->gid;
    for (unsigned i=0;i<d->ngroups;i++) if (d->groups[i].gid!=u->gid && member(d->groups[i].members,u->name)) {
        if (n==16) return false;
        out[n++]=d->groups[i].gid;}
    *count=n;return true;
}
bool db_validate(const userdb_t *d) {
    if (!d->nusers || !d->ngroups || d->nusers!=d->nshadows) return false;
    for (unsigned i=0;i<d->nusers;i++) if (!db_group_id(d,d->users[i].gid) || !db_shadow_name(d,d->users[i].name)) return false;
    for (unsigned i=0;i<d->ngroups;i++) {
        const char *p=d->groups[i].members;while (*p) {char item[32];size_t n=0;
            while (*p && *p!=',') item[n++]=*p++;
            item[n]=0;if (!db_user_name(d,item)) return false;if (*p) p++;}}
    return true;
}
void db_wipe(void *p,size_t n) {volatile unsigned char *q=p;while (n--) *q++=0;}
static const char alphabet[]="./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
static bool cryptchar(char c) {for (unsigned i=0;i<64;i++) if (alphabet[i]==c) return true;return false;}
static bool setting(const char *hash,const char **salt,size_t *ns,uint32_t *rounds,const char **encoded) {
    if (hash[0]!='$' || hash[1]!='5' || hash[2]!='$') return false;
    const char *p=hash+3;*rounds=5000;
    const char prefix[]="rounds=";unsigned i=0;while (i<7 && p[i]==prefix[i]) i++;
    if (i==7) {p+=7;char text[12];size_t n=0;
        while (*p && *p!='$') {if (n==10) return false;text[n++]=*p++;}text[n]=0;
        if (*p++!='$' || !decimal(text,rounds) || *rounds<1000 || *rounds>DB_ROUNDS_MAX) return false;}
    *salt=p;*ns=0;while (*p && *p!='$') {if (!cryptchar(*p++) || ++*ns>16) return false;}
    if (!*ns || *p++!='$') return false;
    if (length(p)!=43) return false;
    for (i=0;i<43;i++) if (!cryptchar(p[i])) return false;
    *encoded=p;return true;
}
bool db_hash_valid(const char *hash) {
    if (!*hash || *hash=='!' || *hash=='*') return true;
    const char *s,*e;size_t n;uint32_t r;return setting(hash,&s,&n,&r,&e);
}
static void add(digest_t *d,const void *p,size_t n) {(void)digest_update(d,p,n);}
static void repeated(digest_t *d,const uint8_t p[32],size_t n) {while (n>=32) {add(d,p,32);n-=32;}add(d,p,n);}
/* SHA-256-crypt per Ulrich Drepper's public specification, using our existing
 * SHA core. Verify a fixed 43-byte encoding without an early-exit comparison. */
bool db_verify(const char *password,const char *hash) {
    if (!password || !hash || *hash=='!' || *hash=='*') return false;
    size_t np=length(password);if (np>DB_PASSWORD_MAX) return false;
    if (!*hash) return !np;
    const char *salt,*encoded;size_t ns;uint32_t rounds;
    if (!setting(hash,&salt,&ns,&rounds,&encoded)) return false;
    uint8_t a[32],b[32],p[32],s[32];digest_t d;
    digest_init(&d,DIGEST_SHA256);add(&d,password,np);add(&d,salt,ns);add(&d,password,np);digest_final(&d,b);
    digest_init(&d,DIGEST_SHA256);add(&d,password,np);add(&d,salt,ns);repeated(&d,b,np);
    for (size_t n=np;n;n>>=1) if (n&1) add(&d,b,32);else add(&d,password,np);
    digest_final(&d,a);
    digest_init(&d,DIGEST_SHA256);for (size_t i=0;i<np;i++) add(&d,password,np);digest_final(&d,p);
    digest_init(&d,DIGEST_SHA256);for (unsigned i=0;i<16u+a[0];i++) add(&d,salt,ns);digest_final(&d,s);
    for (uint32_t i=0;i<rounds;i++) {
        digest_init(&d,DIGEST_SHA256);
        if (i&1) repeated(&d,p,np);else add(&d,a,32);
        if (i%3) repeated(&d,s,ns);
        if (i%7) repeated(&d,p,np);
        if (i&1) add(&d,a,32);else repeated(&d,p,np);
        digest_final(&d,a);
    }
    const uint8_t order[10][3]={{0,10,20},{21,1,11},{12,22,2},{3,13,23},{24,4,14},{15,25,5},{6,16,26},{27,7,17},{18,28,8},{9,19,29}};
    char result[43];unsigned pos=0;
    for (unsigned i=0;i<10;i++) {uint32_t v=((uint32_t)a[order[i][0]]<<16)|((uint32_t)a[order[i][1]]<<8)|a[order[i][2]];
        for (unsigned j=0;j<4;j++) {result[pos++]=alphabet[v&63];v>>=6;}}
    uint32_t v=((uint32_t)a[31]<<8)|a[30];for (unsigned j=0;j<3;j++) {result[pos++]=alphabet[v&63];v>>=6;}
    unsigned diff=0;for (unsigned i=0;i<43;i++) diff|=(unsigned char)result[i]^(unsigned char)encoded[i];
    db_wipe(a,sizeof(a));db_wipe(b,sizeof(b));db_wipe(p,sizeof(p));db_wipe(s,sizeof(s));db_wipe(&d,sizeof(d));db_wipe(result,sizeof(result));
    return diff==0;
}
