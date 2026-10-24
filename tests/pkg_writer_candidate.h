#ifndef PKG_WRITER_CANDIDATE_H
#define PKG_WRITER_CANDIDATE_H
#include "types.h"
/* Private trusted-call-site keys, not user handles or production process refs. */
typedef struct { uint64_t generation;uint32_t slot; } pw_key;
bool pw_register(uint64_t pid,pw_key *out);
bool pw_authorized(pw_key owner);
/* Between durable recovery steps, outside filesystem exclusion. */
bool pw_recovery_step(pw_key owner);
bool pw_begin(pw_key owner,bool durable_idle,uint64_t *session);
bool pw_enter(pw_key owner,uint64_t session,pw_key *operation);
bool pw_complete(pw_key operation);
bool pw_close(pw_key owner,uint64_t session);
bool pw_detach(pw_key owner);
bool pw_exec(pw_key owner);
unsigned pw_references(pw_key owner);
#endif
