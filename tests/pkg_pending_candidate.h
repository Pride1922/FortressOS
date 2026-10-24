#ifndef PKG_PENDING_CANDIDATE_H
#define PKG_PENDING_CANDIDATE_H
#include "pkg_current_candidate.h"
/* Private bounded recovery profile. Observations come from the actual stable
 * filesystem/bitmaps, never from user requests or inferred refund claims. */
typedef struct {
    uint32_t role,inode,generation,mode,uid,gid,links,count;
    uint64_t size,blocks;
    uint8_t hash[32];
    struct {uint32_t logical,length;uint64_t physical;} extent[4];
} pp_object;
typedef struct {
    pc_input authority;
    ps_blob intent;
    const pp_object *objects;size_t object_count;
    const pp_object *executable; /* Present only at published /usr/bin/demo. */
    uint64_t free[2],capacity[2];
} pp_input;
typedef struct {
    uint64_t sequence,state_generation;
    uint32_t phase,installed,publication,action,role,reserved;
    pg_budget budget;
} pp_view;
enum {PP_DONE,PP_DELETE,PP_ACTIVATE,PP_PUBLISH,PP_RETIRE,PP_FINISH};
typedef struct {
    uint8_t grant[1024],owners[8192],account[1024],state[1024];
    uint64_t free_after[2];uint32_t action,role;
} pp_change;
typedef struct {
    ps_workspace baseline;ps_result baseline_result;ps_blob parts[8];
    fpk_verify_context authority;pm_workspace mount;pm_view mounted;
    uint8_t target[1024];
} pp_workspace;
/* No I/O, allocation, transition execution, or publication. Output unchanged
 * on error. Caller supplies disjoint accessible immutable inputs/workspace/out.
 * Bounds: four charged roles, inline dense extents, <=8KiB per object, one demo,
 * unchanged accounts/trust; only closed IDLE and recovery states are accepted. */
int pp_validate(const pp_input *,pp_workspace *,pp_view *);
/* Deterministic next sealed record set. The FS adapter must compare actual
 * planned frees/transfer/rename and journal it with these records atomically. */
int pp_next(const pp_input *,const pp_workspace *,const pp_view *,pp_change *);
#endif
