#ifndef PKG_MOUNT_CANDIDATE_H
#define PKG_MOUNT_CANDIDATE_H
#include "pkg_grant_candidate.h"
#include "pkg_enrollment_candidate.h"
#include "user/pkg/catalog.h"
/* Private bounded single-command profile. Immutable disjoint caller inputs;
 * Fixed records are exact 512/1024/8192-byte accessible buffers. Workspace is
 * disjoint scratch; failure clears its verified bit and preserves out. Actual
 * filesystem identity/refinement is a separate adapter. */
typedef struct {
 const uint8_t *marker,*ledger,*owners,*state,*trust,*pending,*next;
 size_t pending_size,next_size;
 const uint8_t *artifact;size_t artifact_size;
 const uint8_t *account;const pe_enrollment *enrollment;
 const fpk_catalog_policy *policy;
 uint64_t base_version;const uint8_t *platform;
} pm_input;
typedef struct {fpk_verify_context verify;fpk_catalog_policy policy;uint8_t initial_state[1024];} pm_workspace;
typedef struct {pg_view grant;uint64_t state_generation;unsigned installed;} pm_view;
int pm_validate(const pm_input *,pm_workspace *,pm_view *);
#endif
