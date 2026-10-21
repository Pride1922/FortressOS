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
