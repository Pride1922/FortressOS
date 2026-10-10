#include "creds.h"

void creds_init_root(creds_t *out) {
    if (out) *out = (creds_t){.umask = 0022, .cap_effective = CAP_ALL};
}
bool creds_valid(const creds_t *v) {
    if (!v || v->ngroups > CREDS_MAX_GROUPS || v->reserved ||
        (v->umask & ~0777U) || (v->cap_effective & ~CAP_ALL)) return false;
    for (unsigned i = v->ngroups; i < CREDS_MAX_GROUPS; ++i)
        if (v->groups[i]) return false;
    return true;
}
int creds_inherit(const creds_t *old, creds_t *out) {
    if (!out || !creds_valid(old)) return CREDS_EINVAL;
    *out = *old;
    return CREDS_OK;
}
static bool allowed(creds_id_change_t change, uint32_t r, uint32_t e, uint32_t s) {
    return change.keep || change.value == r || change.value == e || change.value == s;
}
static int setids(const creds_t *old, creds_id_change_t r, creds_id_change_t e,
                  creds_id_change_t s, bool uid, creds_t *out) {
    if (!out || !creds_valid(old)) return CREDS_EINVAL;
    uint32_t a = uid ? old->uid : old->gid;
    uint32_t b = uid ? old->euid : old->egid;
    uint32_t c = uid ? old->suid : old->sgid;
    if (!(old->cap_effective & (uid ? CAP_SETUID : CAP_SETGID)) &&
        (!allowed(r,a,b,c) || !allowed(e,a,b,c) || !allowed(s,a,b,c))) return CREDS_EPERM;
    creds_t next = *old;
    if (uid) {
        next.uid = r.keep ? a : r.value;
        next.euid = e.keep ? b : e.value;
        next.suid = s.keep ? c : s.value;
        if (next.uid && next.euid && next.suid) next.cap_effective = 0;
    } else {
        next.gid = r.keep ? a : r.value;
        next.egid = e.keep ? b : e.value;
        next.sgid = s.keep ? c : s.value;
    }
    *out = next;
    return CREDS_OK;
}
int creds_setresuid(const creds_t *old, creds_id_change_t r, creds_id_change_t e,
                    creds_id_change_t s, creds_t *out) {
    return setids(old,r,e,s,true,out);
}
int creds_setresgid(const creds_t *old, creds_id_change_t r, creds_id_change_t e,
                    creds_id_change_t s, creds_t *out) {
    return setids(old,r,e,s,false,out);
}
int creds_setgroups(const creds_t *old, const uint32_t *groups, size_t count, creds_t *out) {
    if (!out || !creds_valid(old) || count > CREDS_MAX_GROUPS || (count && !groups))
        return CREDS_EINVAL;
    if (!(old->cap_effective & CAP_SETGID)) return CREDS_EPERM;
    creds_t next = *old;
    next.ngroups = (uint16_t)count;
    for (unsigned i = 0; i < CREDS_MAX_GROUPS; ++i)
        next.groups[i] = i < count ? groups[i] : 0;
    *out = next;
    return CREDS_OK;
}
int creds_capset(const creds_t *old, uint64_t mask, creds_t *out) {
    if (!out || !creds_valid(old) || (mask & ~CAP_ALL)) return CREDS_EINVAL;
    creds_t next = *old;
    next.cap_effective &= mask;
    *out = next;
    return CREDS_OK;
}
int creds_umask(const creds_t *old, uint32_t mask, creds_t *out) {
    if (!out || !creds_valid(old)) return CREDS_EINVAL;
    creds_t next = *old;
    next.umask = mask & 0777U;
    *out = next;
    return CREDS_OK;
}
