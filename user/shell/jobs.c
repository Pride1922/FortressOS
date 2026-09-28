#include "jobs.h"
#include "io.h"

/* ---- BSS-static job table ---- */
static job_t table[MAX_JOBS];
static int next_job_id = 1;
static int selection[MAX_JOBS];

void jobs_init(void) {
    for (int i = 0; i < MAX_JOBS; i++) {
        selection[i] = -1;
        table[i].in_use = false;
        table[i].state = JOB_STATE_FREE;
    }
    next_job_id = 1;
}

int jobs_next_id(void) {
    for (;;) {
        int id = next_job_id++;
        if (next_job_id > 9999) next_job_id = 1;
        bool used = false;
        for (int i=0; i<MAX_JOBS; ++i)
            if (table[i].in_use && table[i].job_id==id) used=true;
        if (!used) return id;
    }
}

int jobs_alloc(void) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!table[i].in_use) {
            table[i].job_id = jobs_next_id();
            table[i].in_use = true;
            table[i].attrs_valid = false;
            table[i].pgid = 0;
            table[i].member_count = 0;
            table[i].state = JOB_STATE_RUNNING;
            table[i].last_status = 0;
            table[i].notified = false;
            table[i].foreground = false;
            table[i].cmd_text[0] = '\0';
            for (int j = 0; j < MAX_JOB_MEMBERS; j++) {
                table[i].members[j].signal = 0;
                table[i].members[j].stop_signal = 0;
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
    if (!fg) jobs_select(slot);
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

    if (job->member_count && done == job->member_count) {
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
            m->signal = (unsigned)WTERMSIG(status);
            m->exit_status = 128 + (int)WTERMSIG(status);
        } else if (WIFSTOPPED(status)) {
            m->state = JOB_MEM_STOPPED;
            m->stop_signal = (unsigned)WSTOPSIG(status);
        } else if (WIFCONTINUED(status)) {
            m->state = JOB_MEM_RUNNING;
        }

        jobs_recompute_state(slot);
        if (job->state == JOB_STATE_STOPPED && old_state != JOB_STATE_STOPPED) jobs_select(slot);
        return job->state != old_state;
    }
    return false;
}

void jobs_free(int slot) {
    if (slot < 0 || slot >= MAX_JOBS) return;
    for (int i=0; i<MAX_JOBS; ++i) if (selection[i]==slot) {
        for (int k=i; k+1<MAX_JOBS; ++k) selection[k]=selection[k+1];
        selection[MAX_JOBS-1]=-1;
        break;
    }
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

        jobs_print_state(slot, false);
        if (job->state == JOB_STATE_DONE) table[slot].notified = true;
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

void jobs_select(int slot) {
    int pos=MAX_JOBS-1;
    for (int i=0; i<MAX_JOBS; ++i) if (selection[i]==slot) { pos=i; break; }
    for (int i=pos; i>0; --i) selection[i]=selection[i-1];
    selection[0]=slot;
}
static int selected(unsigned nth) {
    for (unsigned pass=0; pass<2; ++pass) for (int i=0; i<MAX_JOBS; ++i) {
        int slot=selection[i];
        const job_t *j=jobs_get(slot);
        if (!j || !j->in_use || j->foreground || j->state==JOB_STATE_DONE) continue;
        if ((j->state==JOB_STATE_STOPPED) != (pass==0)) continue;
        if (!nth--) return slot;
    }
    return -1;
}
int jobs_resolve(const char *spec) {
    if (!spec || equal(spec,"%+") || equal(spec,"%%")) return selected(0);
    if (equal(spec,"%-")) return selected(1);
    if (*spec++!='%' || !*spec) return -2;
    unsigned id=0;
    for (; *spec; ++spec) {
        if (*spec<'0' || *spec>'9' || id>(9999u-(unsigned)(*spec-'0'))/10) return -2;
        id=id*10+(unsigned)(*spec-'0');
    }
    if (!id) return -2;
    for (int i=0; i<jobs_max(); ++i) {
        const job_t *j=jobs_get(i);
        if (j->in_use && j->job_id==(int)id && j->state!=JOB_STATE_DONE) return i;
    }
    return -1;
}
char jobs_marker(int slot) { return slot==selected(0) ? '+' : slot==selected(1) ? '-' : ' '; }
void jobs_mark_running(int slot) {
    if (slot<0 || slot>=MAX_JOBS) return;
    for (int i=0; i<table[slot].member_count; ++i)
        if (table[slot].members[i].state==JOB_MEM_STOPPED) table[slot].members[i].state=JOB_MEM_RUNNING;
    jobs_recompute_state(slot);
}
void jobs_save_attrs(int slot, const terminal_attrs_t *attrs) {
    if (slot<0 || slot>=MAX_JOBS) return;
    table[slot].attrs=*attrs; table[slot].attrs_valid=true;
}
unsigned jobs_term_signal(int slot) {
    const job_t *j=jobs_get(slot);
    if (j) for (int i=0; i<j->member_count; ++i) if (j->members[i].signal) return j->members[i].signal;
    return 0;
}
unsigned jobs_stop_signal(int slot) {
    const job_t *j=jobs_get(slot);
    if (j) for (int i=0; i<j->member_count; ++i)
        if (j->members[i].state==JOB_MEM_STOPPED) return j->members[i].stop_signal;
    return SIGTSTP;
}
void jobs_print_state(int slot, bool markers) {
    const job_t *j=jobs_get(slot);
    if (!j || !j->in_use) return;
    puts("["); put_dec((size_t)j->job_id); puts("]");
    if (markers) { char mark[2]={jobs_marker(slot),0}; puts(mark); puts(" "); }
    else puts("  ");
    if (j->state==JOB_STATE_DONE) {
        unsigned sig=jobs_term_signal(slot);
        if (sig) { puts("Terminated(signal "); put_dec(sig); puts(")"); }
        else { puts("Done"); if (j->last_status) { puts("("); put_dec((size_t)j->last_status); puts(")"); } }
    } else puts(j->state==JOB_STATE_STOPPED ? "Stopped" : "Running");
    puts("                    "); puts(j->cmd_text); puts("\n");
}
static bool live_jobs(void) {
    for (int i=0; i<jobs_max(); ++i) if (table[i].in_use && table[i].state!=JOB_STATE_DONE) return true;
    return false;
}
void jobs_shutdown(void) {
    for (int i=0; i<jobs_max(); ++i) {
        const job_t *j=jobs_get(i);
        if (!j->in_use || j->state==JOB_STATE_DONE) continue;
        (void)call(SYS_KILL,(uintptr_t)-j->pgid,SIGHUP,0);
        (void)call(SYS_KILL,(uintptr_t)-j->pgid,SIGCONT,0);
    }
    /* At exit the shell owns the terminal. Bounded timed reads yield while
     * HUP handlers run; input is discarded because this shell is exiting. */
    static char byte;
    for (unsigned round=0; round<10 && live_jobs(); ++round) {
        jobs_reap_children();
        if (live_jobs()) (void)call(SYS_INPUT_READ,(uintptr_t)&byte,1,100);
    }
    for (int i=0; i<jobs_max(); ++i) {
        const job_t *j=jobs_get(i);
        if (j->in_use && j->state!=JOB_STATE_DONE) (void)call(SYS_KILL,(uintptr_t)-j->pgid,SIGKILL,0);
    }
    /* KILL wakes stopped and blocked peers; collect each owned group. */
    for (int i=0; i<jobs_max(); ++i) {
        const job_t *j=jobs_get(i);
        if (!j->in_use) continue;
        for (;;) {
            uint64_t status;
            long pid=call(SYS_WAITPID,(uintptr_t)-j->pgid,(uintptr_t)&status,0);
            if (pid==SYSCALL_EINTR) continue;
            if (pid<=0) break;
            jobs_update_member(pid,status);
        }
        jobs_free(i);
    }
}
