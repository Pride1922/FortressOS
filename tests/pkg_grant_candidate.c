#include "pkg_grant_candidate.h"
#include "user/tools/digest.h"

static uint64_t le(const uint8_t *p, unsigned n) {
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i) v |= (uint64_t)p[i] << (8*i);
    return v;
}
static int zero(const uint8_t *p, size_t n) {
    uint8_t v = 0;
    for (size_t i = 0; i < n; ++i) v |= p[i];
    return v == 0;
}
static int equal(const uint8_t *a, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; ++i) if (a[i] != b[i]) return 0;
    return 1;
}
int pg_decode(const uint8_t *p, size_t n, pg_view *out) {
    static const uint8_t magic[16] = "FORT-PKG-GRT-v1";
    uint8_t hash[32];
    digest_t ctx;
    pg_view v = {0};
    if (!p || !out || n != 1024 || !equal(p, magic, 16) ||
        le(p+16,4) != 1024 || le(p+20,2) != 1 ||
        le(p+22,2) > PG_RETIRING || !zero(p+350,2) || !zero(p+432,560)) return -1;
    digest_init(&ctx, DIGEST_SHA256);
    if (!digest_update(&ctx,p,992)) return -1;
    digest_final(&ctx,hash);
    if (!equal(hash,p+992,32)) return -1;
    v.phase = (uint16_t)le(p+22,2);
    v.sequence = le(p+24,8); v.epoch = le(p+32,8);
    if (!v.sequence || !v.epoch || zero(p+56,16) || zero(p+72,16) ||
        zero(p+88,16) || zero(p+104,32) || zero(p+168,32) ||
        zero(p+232,32) || zero(p+264,32) || le(p+344,4) != 1 ||
        !le(p+352,4) || !le(p+356,4) || !le(p+360,8) ||
        !le(p+376,8) || !le(p+384,8)) return -1;
    for (unsigned i = 0; i < 2; ++i) {
        v.total[i] = le(p+296+8*i,8);
        v.remaining[i] = le(p+312+8*i,8);
        v.charged[i] = le(p+328+8*i,8);
        if (v.remaining[i] > UINT64_MAX-v.charged[i] ||
            v.total[i] != v.remaining[i]+v.charged[i]) return -1;
    }
    if (v.phase == PG_IDLE) {
        if (le(p+348,2) || !zero(p+40,16) || !zero(p+136,32) ||
            !equal(p+168,p+200,32) || le(p+360,8) != le(p+368,8) ||
            !zero(p+296,48) || !zero(p+392,40)) return -1;
    } else {
        if (zero(p+40,16) || le(p+348,2) < 1 || le(p+348,2) > 3 ||
            zero(p+392,32)) return -1;
        if (v.phase == PG_RESERVED) {
            if (!zero(p+136,32) || !zero(p+200,32) || le(p+368,8) ||
                le(p+424,8)) return -1;
        } else if (zero(p+136,32) || zero(p+200,32) ||
                   le(p+360,8) == UINT64_MAX ||
                   le(p+368,8) != le(p+360,8)+1) return -1;
    }
    *out = v;
    return 0;
}
int pg_encode(const uint8_t *p, size_t n, uint8_t *out, size_t cap) {
    pg_view v;
    if (!out || cap != 1024 || pg_decode(p,n,&v)) return -1;
    for (size_t i = 0; i < 1024; ++i) out[i] = p[i];
    return 0;
}
static int hashed(const uint8_t *p, size_t n, const uint8_t expected[32]) {
    digest_t ctx;
    uint8_t h[32];
    digest_init(&ctx,DIGEST_SHA256);
    if (!digest_update(&ctx,p,n)) return 0;
    digest_final(&ctx,h);
    return equal(h,expected,32);
}
int pg_validate_set(const uint8_t *m, size_t mn, const uint8_t *l, size_t ln,
                    const uint8_t *t, size_t tn, const pg_binding *e, pg_view *out) {
    static const uint8_t mm[16]="FORT-PKG-ENR-v1", tm[16]="FORT-PKG-OWN-v1";
    pg_view v;
    uint64_t charged[2]={0,0};
    unsigned previous=0;
    if (!m || !l || !t || !e || !out || mn!=512 || tn!=8192 ||
        pg_decode(l,ln,&v) || !equal(m,mm,16) || le(m+16,4)!=512 ||
        le(m+20,2)!=1 || le(m+22,2) || !e->epoch || le(m+24,8)!=e->epoch ||
        !zero(m+176,304) || le(m+168,4)!=2 || le(m+172,4)!=1 ||
        !hashed(m,480,m+480) || !hashed(m,512,e->marker_sha256)) return -1;
    for (unsigned i=0;i<5;++i)
        if (zero(e->uuid[i],16) || !equal(m+32+16*i,e->uuid[i],16)) return -1;
    if (zero(e->profile,32) || !equal(m+112,e->profile,32)) return -1;
    for (unsigned i=0;i<3;++i) {
        if (!e->inode[i] || !e->generation[i] ||
            le(m+144+8*i,4)!=e->inode[i] || le(m+148+8*i,4)!=e->generation[i]) return -1;
        for (unsigned j=0;j<i;++j) if (e->inode[j]==e->inode[i]) return -1;
    }
    if (v.epoch!=e->epoch || !equal(l+56,e->uuid[4],16) ||
        !equal(l+72,e->uuid[0],16) || !equal(l+88,e->uuid[3],16) ||
        !equal(l+104,e->profile,32) || le(l+352,4)!=e->inode[2] ||
        le(l+356,4)!=e->generation[2] || !equal(t,tm,16) ||
        le(t+16,4)!=8192 || le(t+20,2)!=1 || le(t+22,2)>64 ||
        le(t+24,8)!=e->epoch || !equal(t+32,l+40,16) ||
        le(t+48,8)!=v.sequence || !equal(t+56,e->uuid[4],16) ||
        !equal(t+72,e->uuid[0],16) || !equal(t+88,e->uuid[3],16) ||
        !equal(t+104,e->profile,32) || !zero(t+136,120) ||
        !zero(t+4352,3808) || !hashed(t,8160,t+8160) ||
        !hashed(t,8192,l+264)) return -1;
    unsigned count=(unsigned)le(t+22,2);
    if (v.phase==PG_IDLE && count) return -1;
    for (unsigned i=0;i<64;++i) {
        const uint8_t *p=t+256+64*i;
        if (i>=count) { if (!zero(p,64)) return -1; continue; }
        unsigned role=(unsigned)le(p,2), cls=(unsigned)le(p+2,2);
        /* Private role profile: 1..16 payload,17 activation,18 next state,
         * 19 intent,20 documentation;21..36 old inventory victims. */
        if (role<=previous || role>36 || cls<1 || cls>3 ||
            !le(p+4,4) || !le(p+8,4) || !equal(p+16,l+40,16) ||
            !zero(p+56,8)) return -1;
        previous=role;
        if (cls==3) {
            if (!zero(p+12,4) || !zero(p+32,24)) return -1;
        } else if ((cls==1 && role>20) || (cls==2 && role<=20) ||
                   (le(p+12,4)!=1 && le(p+12,4)!=2) || le(p+48,8)!=1) return -1;
        for (unsigned j=0;j<3;++j) if (le(p+4,4)==e->inode[j]) return -1;
        for (unsigned j=0;j<i;++j)
            if (le(p+4,4)==le(t+256+64*j+4,4)) return -1;
        if (cls==1) for (unsigned d=0;d<2;++d) {
            uint64_t n=le(p+40+8*d,8);
            if (n>UINT64_MAX-charged[d]) return -1;
            charged[d]+=n;
        }
    }
    if (charged[0]!=v.charged[0] || charged[1]!=v.charged[1]) return -1;
    *out=v;
    return 0;
}
int pg_budget_valid(const pg_budget *b) {
    if (!b || !b->sequence) return 0;
    for (unsigned i = 0; i < 2; ++i) {
        if (b->remaining[i] > UINT64_MAX-b->charged[i] ||
            b->total[i] != b->remaining[i]+b->charged[i] ||
            b->free[i] < b->remaining[i] ||
            b->free[i]-b->remaining[i] < b->reserve[i] ||
            b->free[i] > b->capacity[i] ||
            b->charged[i] > b->capacity[i]-b->free[i] ||
            b->baseline[i] != b->capacity[i]-b->free[i]-b->charged[i]) return 0;
    }
    return 1;
}
int pg_plan(const pg_budget *b, unsigned op, const uint64_t d[2], pg_budget *out) {
    pg_budget v;
    if (!out || !d || !pg_budget_valid(b) || op > PG_ORDINARY ||
        b->sequence == UINT64_MAX || (!d[0] && !d[1])) return -1;
    v = *b;
    for (unsigned i = 0; i < 2; ++i) {
        uint64_t x = d[i];
        switch (op) {
        case PG_CHARGE:
            if (x > v.remaining[i]) return -1;
            v.free[i]-=x; v.remaining[i]-=x; v.charged[i]+=x; break;
        case PG_REFUND:
            if (x > v.charged[i]) return -1;
            v.free[i]+=x; v.remaining[i]+=x; v.charged[i]-=x; break;
        case PG_TRANSFER:
            if (x > v.charged[i]) return -1;
            v.charged[i]-=x; v.total[i]-=x; v.baseline[i]+=x; break;
        case PG_RELEASE:
            if (x > v.remaining[i]) return -1;
            v.remaining[i]-=x; v.total[i]-=x; break;
        case PG_BASE_FREE:
            if (x > v.baseline[i]) return -1;
            v.free[i]+=x; v.baseline[i]-=x; break;
        case PG_ORDINARY:
            if (x > v.free[i]) return -1;
            v.free[i]-=x; v.baseline[i]+=x; break;
        }
    }
    ++v.sequence;
    if (!pg_budget_valid(&v)) return -1;
    *out = v;
    return 0;
}
int pg_reclaim(const pg_budget *b,const uint64_t f[2],const uint64_t q[2],pg_budget *out) {
    uint64_t amount[2];pg_budget next;
    if (!out || !f || !q || !pg_budget_valid(b) || b->sequence==UINT64_MAX) return -1;
    for (unsigned i=0;i<2;i++) {
        if (q[i]>b->charged[i] || f[i]<b->free[i] ||
            b->charged[i]-q[i]!=f[i]-b->free[i]) return -1;
        amount[i]=b->charged[i]-q[i];
    }
    if (amount[0] || amount[1]) {
        if (pg_plan(b,PG_REFUND,amount,&next)) return -1;
    } else { next=*b;next.sequence++; }
    *out=next;return 0;
}
