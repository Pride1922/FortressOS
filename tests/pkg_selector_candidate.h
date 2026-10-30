#ifndef PKG_SELECTOR_CANDIDATE_H
#define PKG_SELECTOR_CANDIDATE_H
#include "pkg_mount_candidate.h"
#define PS_BODY 768u
#define PS_MAX (PS_BODY+8u*96u)
#define PS_BUNDLE_MAX 32768u
/* Private TEST role only. Trusted policy is supplied independently of the
 * inspected selector/root. No key loading, boot activation or target I/O. */
typedef struct {
 uint8_t selector_keys[8][32],enrollment_keys[8][32],catalog_keys[8][32];
 uint32_t selector_count,selector_threshold,enrollment_count,enrollment_threshold,catalog_count,catalog_threshold;
 uint8_t uuid[5][16],profile[32],platform[32],kernel[32],bootstrap[32];
 uint8_t root_hash[32],catalog_hash[32],selector_floor_hash[32];
 uint64_t now,epoch_floor,root_version,catalog_revision_floor,base_version;
} ps_policy;
typedef struct {const uint8_t *p;size_t n;} ps_blob;
typedef struct {
 ps_blob parts[8];pe_policy enrollment_policy;pe_enrollment enrollment;
 fpk_catalog_policy catalog_policy;pm_workspace mount;pm_view view;
} ps_workspace;
typedef struct {uint64_t epoch;uint8_t selector_sha256[32],bundle_sha256[32];} ps_result;
/* Output/workspace cannot overlap either immutable input or policy/each other.
 * Output is preserved on error. Workspace is caller-owned bounded scratch. */
int ps_validate(const uint8_t *,size_t,const uint8_t *,size_t,const ps_policy *,ps_workspace *,ps_result *);
enum {PS_ABSENT,PS_PRESENT,PS_IO_ERROR,PS_PARTIAL};
enum {PS_REFUSE,PS_LEGACY,PS_PROTECTED};
/* Only independently verified absence of ALL reserved artifacts admits legacy.
 * A required selector's absence is never legacy; invalid signatures never pass. */
unsigned ps_admission(bool required,unsigned artifacts,bool valid);
#endif
