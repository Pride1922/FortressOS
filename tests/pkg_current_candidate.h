#ifndef PKG_CURRENT_CANDIDATE_H
#define PKG_CURRENT_CANDIDATE_H
#include "pkg_selector_candidate.h"
/* Private read-only endpoint proof. Genesis and one completed IDLE demo are
 * supported; no transition execution, account change, rotation or mutation. */
typedef struct {
    ps_blob selector,baseline,current,artifact,executable;
    const ps_policy *policy;
    uint64_t grant_floor,state_floor; /* Independent trusted embedding input. */
} pc_input;
typedef struct {
    ps_workspace baseline;
    ps_blob parts[8];
    pm_workspace current;
    pm_view view;
} pc_workspace;
typedef struct {
    ps_result enrollment;
    uint64_t grant_sequence,state_generation,credential_generation;
    uint32_t installed,reserved;
} pc_result;
/* Workspace belongs in caller-owned BSS/heap. Output is unchanged on error.
 * Validated buffers/policy/input object must be disjoint from output/workspace.
 * Current bytes are not compared to the immutable genesis bundle hash. Actual
 * file identities/maps/modes/namespaces remain a filesystem adapter obligation. */
int pc_validate(const pc_input *,pc_workspace *,pc_result *);
#endif
