#ifndef PKG_MUTATION_CANDIDATE_H
#define PKG_MUTATION_CANDIDATE_H
#include "pkg_writer_candidate.h"
#define PM_CAPACITY 8u
typedef struct {uint64_t generation;unsigned slot;} pm_key;
enum pm_kind {PM_ORDINARY=1,PM_PACKAGE,PM_ACCOUNT};
typedef struct {uint64_t mount;enum pm_kind kind;} pm_view;
/* Trusted internal entry points, never a user-supplied lane or credential ABI.
 * ACCOUNT is issued only by the account-service adapter. Package admission
 * retains the real private writer operation. No callback runs under the guard.
 * All calls require no filesystem/process/scheduler lock on entry. */
bool pm_configure(uint64_t mount);
int pm_ordinary(uint64_t mount,pm_key *out);
int pm_account(uint64_t mount,pm_key *out);
int pm_package(uint64_t mount,pw_key owner,uint64_t session,pm_key *out);
/* Snapshot an exclusively owned, unpinned admission before binding transfer. */
bool pm_describe(pm_key key,uint64_t mount,pm_view *out);
const pm_view *pm_pin(pm_key key,uint64_t mount);
bool pm_unpin(pm_key key);
bool pm_finish(pm_key key);
void pm_counts(unsigned *admitted,unsigned *pinned,bool *account_pending);
#endif
