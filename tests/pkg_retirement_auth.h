#ifndef PKG_RETIREMENT_AUTH_H
#define PKG_RETIREMENT_AUTH_H
#include "pkg_retirement_candidate.h"
#include "pkg_mount_candidate.h"
#define PA_BODY 1024u
#define PA_MAX (PA_BODY+8u*96u)
/* Independent, caller-owned retirement-role trust, never selected by disk.
 * Public deterministic fixture keys only; no production signer/provisioner. */
typedef struct {
    uint8_t keys[8][32],certificate_sha256[32];
    unsigned key_count,threshold;
    const pe_policy *enrollment;
    const fpk_catalog_policy *catalog;
    const uint8_t *platform;
    uint64_t base_version;
} pa_policy;
typedef struct {
    pr_set records;
    const uint8_t *certificate,*enrollment,*state,*trust,*intent,*artifact,*executable;
    size_t certificate_n,enrollment_n,artifact_n,executable_n;
} pa_input;
/* Workspace and output must be disjoint from immutable accessible inputs and
 * policy. Caller retains those inputs throughout the call; no I/O or allocation.
 * Large workspace belongs in BSS/heap, not a kernel stack. Serialized calls. */
typedef struct {
    pm_workspace mount;
    pe_enrollment enrollment;
    pr_binding binding;
    pr_view view;
    uint8_t account_v3[1024],initial_grant[1024],initial_owners[8192],anchor[1024];
} pa_workspace;
typedef struct {pr_binding binding;pr_view view;} pa_result;
/* Recovery-only, existing RETIRING/install, selected charged archive role 1.
 * Verifies signatures and exact retained evidence; does not prove physical maps,
 * account database coherence, durable enrollment publication, antirollback,
 * writer lifetime, journal atomicity or pre-replay mount admission.
 * On failure output is preserved and workspace.mount.verify.verified is zero. */
int pa_verify(const pa_input *,const pa_policy *,pa_workspace *,pa_result *);
#endif
