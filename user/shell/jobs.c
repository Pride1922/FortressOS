#include "jobs.h"
#include "io.h"

/* ---- BSS-static job table ---- */
static job_t table[MAX_JOBS];
static int next_job_id = 1;

void jobs_init(void) {
    for (int i = 0; i < MAX_JOBS; i++) {
        table[i].in_use = false;
        table[i].state = JOB_STATE_FREE;
    }
    next_job_id = 1;
}

int jobs_next_id(void) {
    int id = next_job_id++;
    if (next_job_id > 9999) next_job_id = 1;
    return id;
}

int jobs_alloc(void) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!table[i].in_use) {
            table[i].in_use = true;
            table[i].job_id = jobs_next_id();
            table[i].pgid = 0;
            table[i].member_count = 0;
            table[i].state = JOB_STATE_RUNNING;
            table[i].last_status = 0;
            table[i].notified = false;
            table[i].foreground = false;
            table[i].cmd_text[0] = '\0';
            for (int j = 0; j < MAX_JOB_MEMBERS; j++) {
                table[i].members[j].pid = 0;
                table[i].members[j].state = JOB_MEM_NONE;
                table[i].members[j].exit_status = 0;
            }
            return i;
        }
    }
    return -1;
}

void jobs_set_pgid(int slot, long pgid) {
    if (slot < 0 || slot >= MAX_JOBS) return;
    table[slot].pgid = pgid;
}

void jobs_set_cmd(int slot, const char *text) {
    if (slot < 0 || slot >= MAX_JOBS || !text) return;
    size_t i = 0;
    while (text[i] && i < MAX_JOB_CMD_LEN - 1) {
        table[slot].cmd_text[i] = text[i];
        i++;
    }
    table[slot].cmd_text[i] = '\0';
}

void jobs_add_member(int slot, long pid) {
    if (slot < 0 || slot >= MAX_JOBS) return;
    job_t *job = &table[slot];
    if (job->member_count >= MAX_JOB_MEMBERS) return;
    job_member_t *m = &job->members[job->member_count];
    m->pid = pid;
    m->state = JOB_MEM_RUNNING;
    m->exit_status = 0;
    job->member_count++;
}

void jobs_set_foreground(int slot, bool fg) {
    if (slot < 0 || slot >= MAX_JOBS) return;
    table[slot].foreground = fg;
}

int jobs_find_by_pgid(long pgid) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (table[i].in_use && table[i].pgid == pgid) return i;
    }
    return -1;
}

int jobs_find_by_pid(long pid) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!table[i].in_use) continue;
        for (int j = 0; j < table[i].member_count; j++) {
            if (table[i].members[j].pid == pid) return i;
        }
    }
    return -1;
}

void jobs_recompute_state(int slot) {
    if (slot < 0 || slot >= MAX_JOBS || !table[slot].in_use) return;
    job_t *job = &table[slot];

    int running = 0, stopped = 0, done = 0;
    for (int i = 0; i < job->member_count; i++) {
        switch (job->members[i].state) {
        case JOB_MEM_RUNNING: running++; break;
        case JOB_MEM_STOPPED: stopped++; break;
        case JOB_MEM_DONE:    done++;    break;
        default: break;
        }
    }

    if (done == job->member_count) {
        job->state = JOB_STATE_DONE;
        /* Last stage's exit status determines the job's status. */
        job->last_status = job->members[job->member_count - 1].exit_status;
    } else if (running == 0 && stopped > 0) {
        /* Every remaining (non-exited) member is stopped. */
        job->state = JOB_STATE_STOPPED;
    } else {
        job->state = JOB_STATE_RUNNING;
    }
}

bool jobs_update_member(long pid, uint64_t status) {
    int slot = jobs_find_by_pid(pid);
    if (slot < 0) return false;
    job_t *job = &table[slot];

    for (int i = 0; i < job->member_count; i++) {
        if (job->members[i].pid != pid) continue;
        job_member_t *m = &job->members[i];
        job_state_t old_state = job->state;

        if (WIFEXITED(status)) {
            m->state = JOB_MEM_DONE;
            m->exit_status = (int)WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            m->state = JOB_MEM_DONE;
            m->exit_status = 128 + (int)WTERMSIG(status);
        } else if (WIFSTOPPED(status)) {
            m->state = JOB_MEM_STOPPED;
        } else if (WIFCONTINUED(status)) {
            m->state = JOB_MEM_RUNNING;
        }

        jobs_recompute_state(slot);
        return job->state != old_state;
    }
    return false;
}

void jobs_free(int slot) {
    if (slot < 0 || slot >= MAX_JOBS) return;
    table[slot].in_use = false;
    table[slot].state = JOB_STATE_FREE;
}

const job_t *jobs_get(int slot) {
    if (slot < 0 || slot >= MAX_JOBS) return 0;
    return &table[slot];
}

int jobs_max(void) {
    return MAX_JOBS;
}

/* ---- Reaping and notification ---- */

static bool reap_children(bool at_prompt) {
    bool printed = false;
    for (;;) {
        uint64_t status = 0;
        long pid = call(SYS_WAITPID, (uintptr_t)(int64_t)-1,
                        (uintptr_t)&status, WNOHANG | WUNTRACED | WCONTINUED);
        if (pid == SYSCALL_EINTR) continue;
        if (pid <= 0) break;

        bool transition = jobs_update_member(pid, status);
        if (!transition) continue;

        int slot = jobs_find_by_pid(pid);
        if (slot < 0) continue;
        const job_t *job = &table[slot];

        /* Only print notifications for background jobs. */
        if (job->foreground) continue;

        if (job->state != JOB_STATE_DONE && job->state != JOB_STATE_STOPPED)
            continue;
        /* Leave the active edit line intact; UI repaints it after the batch. */
        if (at_prompt && !printed) puts("\n");
        printed = true;

        if (job->state == JOB_STATE_DONE) {
            puts("[");
            put_dec((size_t)job->job_id);
            puts("]  Done");
            if (job->last_status) {
                puts("(");
                put_dec((size_t)job->last_status);
                puts(")");
            }
            puts("                    ");
            puts(job->cmd_text);
            puts("\n");
            table[slot].notified = true;
        } else if (job->state == JOB_STATE_STOPPED) {
            puts("[");
            put_dec((size_t)job->job_id);
            puts("]  Stopped                 ");
            puts(job->cmd_text);
            puts("\n");
        }
    }
    return printed;
}

void jobs_reap_children(void) {
    (void)reap_children(false);
}

bool jobs_reap_prompt(void) {
    bool printed = reap_children(true);
    jobs_gc();
    return printed;
}

void jobs_print_launch(int slot) {
    if (slot < 0 || slot >= MAX_JOBS || !table[slot].in_use) return;
    const job_t *job = &table[slot];
    puts("[");
    put_dec((size_t)job->job_id);
    puts("] ");
    put_dec((size_t)job->pgid);
    puts("\n");
}

void jobs_gc(void) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (table[i].in_use && table[i].state == JOB_STATE_DONE && table[i].notified) {
            jobs_free(i);
        }
    }
}
