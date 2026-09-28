#ifndef SHELL_JOBS_H
#define SHELL_JOBS_H

#include "types.h"
#include "syscall_abi.h"
#include "terminal.h"

/* Bounded BSS job table for S8 Phase 4.
 * All storage is file-scope static; nothing on the stack. */

#define MAX_JOBS        8
#define MAX_JOB_MEMBERS 8
#define MAX_JOB_CMD_LEN 80

/* Per-member state machine: RUNNING → STOPPED or DONE; STOPPED → RUNNING or DONE. */
typedef enum {
    JOB_MEM_NONE = 0,
    JOB_MEM_RUNNING,
    JOB_MEM_STOPPED,
    JOB_MEM_DONE
} job_member_state_t;

/* Aggregate job state. */
typedef enum {
    JOB_STATE_FREE = 0,     /* Slot unused */
    JOB_STATE_RUNNING,      /* At least one member running */
    JOB_STATE_STOPPED,      /* All remaining members stopped */
    JOB_STATE_DONE,         /* All members exited */
    JOB_STATE_NOTIFIED      /* Done, user notified, eligible for reclaim */
} job_state_t;

typedef struct {
    long pid;
    job_member_state_t state;
    unsigned signal, stop_signal;
    int exit_status;        /* Only valid when state == DONE */
} job_member_t;

typedef struct {
    bool in_use;
    int job_id;             /* Small monotonic counter (1-based, wraps at 9999) */
    long pgid;              /* Process group ID of this job */
    int member_count;
    job_member_t members[MAX_JOB_MEMBERS];
    job_state_t state;
    int last_status;        /* Exit status of the final pipeline stage */
    terminal_attrs_t attrs;
    bool attrs_valid;
    bool notified;          /* User saw the Done/Stopped notification */
    bool foreground;        /* True if this is the foreground job */
    char cmd_text[MAX_JOB_CMD_LEN];  /* Bounded command text for display */
} job_t;

/* Initialise the table (called once from shell_main). */
void jobs_init(void);

/* Reserve a slot. Returns the slot index (0..MAX_JOBS-1) or -1 if full. */
int jobs_alloc(void);

/* Fill in a reserved slot's metadata. */
void jobs_set_pgid(int slot, long pgid);
void jobs_set_cmd(int slot, const char *text);
void jobs_add_member(int slot, long pid);
void jobs_set_foreground(int slot, bool fg);

/* Look up a job slot by pgid. Returns slot index or -1. */
int jobs_find_by_pgid(long pgid);

/* Look up a job slot containing a specific member PID. Returns slot index or -1. */
int jobs_find_by_pid(long pid);

/* Update member state from a waitpid report. Returns true if a state transition
 * occurred that warrants a notification (job newly stopped or newly done). */
bool jobs_update_member(long pid, uint64_t status);

/* Recalculate aggregate state from member states. */
void jobs_recompute_state(int slot);

/* Free a slot (after notification and display). */
void jobs_free(int slot);

/* Access the table for display / iteration. */
const job_t *jobs_get(int slot);
int jobs_max(void);

/* Get and bump the monotonic job ID counter. */
int jobs_next_id(void);

/* Drain waitpid reports: call from the main loop before prompting and after
 * SIGCHLD. Updates job table; prints notifications for background jobs. */
void jobs_reap_children(void);

/* Main-context idle drain, with a newline before the first notification and
 * GC after the batch. Returns true only if output requires an editor repaint.
 * Does not change signal masks; call on every input timeout and EINTR. */
bool jobs_reap_prompt(void);

/* Print [N] PID for a newly backgrounded job. */
void jobs_print_launch(int slot);

/* Reclaim notified-done slots (housekeeping after prompt). */
void jobs_gc(void);

/* Selection: stopped jobs first, then background jobs, newest in each class. */
void jobs_select(int slot);
int jobs_resolve(const char *spec); /* NULL means %+; -1 missing, -2 malformed */
char jobs_marker(int slot);
void jobs_mark_running(int slot);
void jobs_save_attrs(int slot, const terminal_attrs_t *attrs);
unsigned jobs_term_signal(int slot);
unsigned jobs_stop_signal(int slot);
void jobs_print_state(int slot, bool markers);
void jobs_shutdown(void);
#endif /* SHELL_JOBS_H */
