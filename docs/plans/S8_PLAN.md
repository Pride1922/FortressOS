Shell Milestone S8: Jobs, Signals, and Process Groups
Status: proposed; not implemented. Prepared 2026-09-27 after Shell Milestone S7 completion.
Dependencies: S7 complete (pipes, stream utilities, builtin pipeline stages).
Scope boundary: Job control for interactive use; signal delivery for the signals job control needs. Full POSIX signal semantics (real-time signals, signal queues, sigwait, sigsuspend) are deferred.

1. Reading Pass — Do This First
Before writing any code, confirm the current code shape:
Current process model — thread.h, thread.c. Does the TCB have a process group field? A session ID? A controlling-terminal field? Is there a setpgid equivalent anywhere?
Current terminal model — input.c, console.c. Is there a notion of a "foreground process group" for the terminal? When the keyboard IRQ fires, who receives the resulting bytes?
Current signal support — search for SIG, signal, sigaction, kill. From the S7 work, there is no signal delivery. Confirm this and identify where it would be added.
Current SYS_WAIT semantics — sys_wait in syscall.c, process_wait_child in thread.c. Does it distinguish exit from stop? Does it support WUNTRACED? What does it return when a child is stopped?
Current SYS_SPAWN_EXT fd-action model — does it have any place to express "put this child in a new process group"? The pipeline executor in S7 creates N stages; each needs to be in the same new process group.
Current shell job model — is there any notion of a "job" in user/shell/? The pipeline executor's static stages[] array is per-invocation, not a persistent job table.
Report the reading-pass findings. If any of the six items reveals a structural conflict (e.g., the scheduler has no way to stop a thread cleanly, or the terminal has no notion of a foreground group), stop and report before designing.

2. Features That Should Not Be Missing
These are the load-bearing features. If any is omitted, S8 is not a job-control shell.

2.1 Process Groups
Every process must belong to a process group. A process group is a set of processes that can be signaled as a unit .
setpgid(pid, pgid) — join or create a process group 
getpgrp() — read the current process's group 
A pipeline is one process group. All N stages share the same pgid.
The shell itself is in its own process group, distinct from its children 
Why it matters: Ctrl+C must kill the whole pipeline, not just the first stage. Process groups are how the terminal driver knows which processes to signal .

2.2 Controlling Terminal and Foreground Group
A session has one controlling terminal. The terminal has a foreground process group; keyboard-generated signals go to that group .
tcsetpgrp(fd, pgid) — set the terminal's foreground group 
tcgetpgrp(fd) — read it 
Only the foreground group may read from the terminal. A background process attempting to read gets SIGTTIN, which stops it unless caught 
Why it matters: Background jobs must not steal input. The shell must hand the terminal to the foreground job and take it back when the job stops or exits .

2.3 Job Control Signals
These are the signals that make job control work:

Signal	Trigger	Effect
SIGINT	Ctrl+C	Terminate foreground group
SIGTSTP	Ctrl+Z	Stop foreground group
SIGTTIN	Background read	Stop background process
SIGTTOU	Background write (if TOSTOP)	Stop background process
SIGCONT	fg/bg/kill -CONT	Resume a stopped group
SIGCHLD	Child stops/exits	Notify the shell
The shell must ignore the stop signals on itself so it cannot stop itself .

2.4 Background Execution (&)
cmd & must:
Start the command in its own process group
Not put it in the foreground
Return the prompt immediately
Track it in the job table

2.5 Job Table and Job Control Builtins
jobs — list active jobs with job numbers, state (Running/Stopped), and command
fg [%n] — bring job n to the foreground, SIGCONT it, tcsetpgrp it, wait for it
bg [%n] — continue job n in the background
%n, %+, %- job specifiers

2.6 Stop and Continue
Ctrl+Z sends SIGTSTP to the foreground group; all processes in the job stop 
The shell regains the terminal when the job stops 
fg sends SIGCONT to the group before waiting 
Stopped jobs must be tracked and resumed

2.7 SIGCHLD and Wait-for-Stop
When a foreground job stops or terminates, the shell is notified via waitpid() with WUNTRACED set . The shell needs to distinguish:
Exited normally
Exited via signal
Stopped (WIFSTOPPED)
The current SYS_WAIT may need extension to carry this information.
2.8 Terminal Mode Save/Restore
When a foreground job stops, the shell must save the terminal's current settings and restore them when the job is continued . Applications like vi or less change terminal modes; the shell must not clobber them.

3. Features That Can Be Deliberately Deferred
These are real POSIX features, but not required for a working job-control shell. Defer them and document the deferral.
3.1 Full Session Management (setsid)
setsid() creates a new session and detaches from the controlling terminal . This is needed for daemons and login management, not for interactive job control. Defer.
3.2 Orphaned Process Group Semantics
The POSIX orphaned-process-group rules prevent stopped processes from being stranded without a parent to continue them . This is a real safety property, but it only matters in edge cases (shell exits with stopped jobs). Defer, and document that stopped jobs are killed on shell exit as a simpler alternative.
3.3 SIGSTOP vs SIGTSTP
SIGSTOP cannot be caught or ignored; SIGTSTP can. For interactive Ctrl+Z, SIGTSTP is correct . SIGSTOP via kill is useful but not essential. Defer SIGSTOP unless it's trivial.
3.4 TOSTOP Terminal Mode
By default, background processes may write to the terminal. TOSTOP makes background writes also trigger SIGTTOU . This is a niche setting. Defer.
3.5 Job Control in Non-Interactive Shells
A non-interactive shell (script) must not enable job control . This is a small conditional, but the full behavior (leaving all children in the shell's group) can be simplified: just don't call setpgid in non-interactive mode.
3.6 SIGPIPE Full Semantics
S7's work used "write returns EPIPE, tool exits 141" as a simulation. S8 should replace this with actual signal delivery for SIGPIPE. This is worth doing because it's the one signal that external programs already expect. But if the scope is tight, deferring it is defensible.

4. Proposed Phases
Phase 1 — Kernel: Process Groups and Sessions
Scope:
Add pgid and sid fields to the TCB
SYS_SETPGID, SYS_GETPGRP syscalls
setpgid(pid, pgid) semantics: join existing group or create new one 
Process group inheritance on spawn (child inherits parent's group by default)
Basic validation: cannot move a process to a group in a different session
Verification: Kernel self-tests: create group, join group, validate cross-group rejection, verify pgid inheritance.

Phase 2 — Kernel: Signal Delivery and Stop/Continue
Scope:
Signal state in the TCB: pending mask, blocked mask, handler table
SYS_KILL(pid, sig) — send a signal to a process or process group (-pgid)
SYS_SIGACTION(sig, handler) — install/read a handler
SYS_SIGPROCMASK — block/unblock signals
Default actions: SIG_DFL (term/stop/ignore per signal), SIG_IGN
Stop/continue: SIGTSTP, SIGSTOP, SIGCONT — thread transitions to a STOPPED state, removed from runqueue, re-added on SIGCONT
Delivery timing: check pending signals on return to user mode (syscall exit, interrupt return)
Verification: Kernel self-tests: send signal to self, catch and return, default terminate, stop/continue cycle, group signal delivery.

Phase 3 — Kernel: Terminal Foreground Group
Scope:
Terminal state: foreground_pgid
SYS_TCSETPGRP(fd, pgid) — set the terminal's foreground group
SYS_TCGETPGRP(fd) — read it
Keyboard-generated signals: when Ctrl+C / Ctrl+Z are received, the terminal driver sends the signal to the foreground group 
SIGTTIN on background read: if a process not in the foreground group attempts to read from the terminal, stop it 
Verification: Kernel self-tests: set foreground group, verify signal goes to the correct group, verify SIGTTIN on background read.

Phase 4 — Shell: Job Table and Background Execution
Scope:
Shell-side job table (bounded, BSS)
& operator in the parser and executor
Job launch: fork children, setpgid(child, child), add to job table
Foreground launch: tcsetpgrp(tty, child_pgid) before waiting 
After the job stops or exits: tcsetpgrp(tty, shell_pgid) 
Verification: Host tests for job table; QEMU integration: sleep 5 & returns prompt, jobs shows it, fg %1 waits for it.

Phase 5 — Shell: jobs, fg, bg, and Job Specifiers
Scope:
jobs builtin — list jobs with %n, state, command
fg %n — foreground the job, SIGCONT, tcsetpgrp, wait 
bg %n — send SIGCONT, leave in background
Job specifiers: %n, %+ (current), %- (previous)
Verification: QEMU integration: full cycle sleep 100 & → jobs → fg %1 → Ctrl+Z → bg %1 → kill %1.

Phase 6 — Shell: Ctrl+C, Ctrl+Z, and Terminal Mode Save/Restore

Scope:
Ctrl+C: terminal driver sends SIGINT to the foreground group; shell ignores it when at the prompt (cancels the current line)
Ctrl+Z: terminal driver sends SIGTSTP to the foreground group; the job stops; the shell regains the terminal 
Terminal mode save/restore: when a foreground job stops, save the terminal settings; restore them when the job is continued 
Shell sets its own handlers for SIGINT, SIGTSTP, SIGTTIN, SIGTTOU to SIG_IGN 
Verification: QEMU integration: run a long job, Ctrl+Z, verify prompt returns and job is Stopped; fg resumes; terminal modes are correct.

Phase 7 — SIGPIPE Real Signal Delivery
Scope:
Replace the "return EPIPE, exit 141" simulation from S7 with actual SIGPIPE delivery
When a write to a pipe with no readers occurs, deliver SIGPIPE to the writing process
Default action: terminate
If the process catches/ignores SIGPIPE, the write returns EPIPE
Verification: head -c 1 closing a pipe; producer receives SIGPIPE and dies; if it ignores SIGPIPE, it gets EPIPE and exits 141.

5. Acceptance Matrix
Gate	Target	Criteria
P1 — Process groups	Kernel self-tests	setpgid join/create, inheritance, cross-session rejection
P2 — Signal delivery	Kernel self-tests	kill to process and group, SIG_IGN, SIG_DFL, custom handler, stop/continue
P3 — Terminal foreground	Kernel self-tests	tcsetpgrp/tcgetpgrp, Ctrl+C goes to foreground group, SIGTTIN on background read
P4 — Background execution	QEMU integration	cmd &, prompt returns, jobs shows job
P5 — Job control builtins	QEMU integration	fg, bg, jobs, %n specifiers
P6 — Ctrl+C / Ctrl+Z	QEMU integration	Ctrl+C kills foreground, Ctrl+Z stops, prompt returns, fg resumes, terminal modes restored
P7 — SIGPIPE	QEMU integration	Producer dies on SIGPIPE, ignores-catcher gets EPIPE
Regression	test-shell-s7, test-smp-append, test-shell-s6-resources	All prior behavior unchanged
Hardware	Dell acceptance	sleep 10 &, jobs, fg, Ctrl+C, Ctrl+Z on physical hardware

6. Scope Limitations
No setsid — session management for daemons and login is out of scope
No orphaned-group semantics — shell exit with stopped jobs kills them
No SIGSTOP — SIGTSTP via Ctrl+Z only
No TOSTOP — background writes allowed by default
No job control in non-interactive shells — simplified to "don't call setpgid"
No SIGCHLD handler — shell uses synchronous waitpid with WUNTRACED

7. Risks
Highest risk: kernel stop/continue semantics. Stopping a thread that holds a spinlock (or is in the middle of a syscall) requires care. The thread must be stopped at a clean point — most naturally, on return from the kernel to user mode. This is analogous to signal delivery timing and should be designed with the same discipline.
Second risk: terminal foreground group and the retained FD 31. The shell's private UI terminal handle (FD 31 from S6 Phase 4D) must not be affected by tcsetpgrp. It's a separate handle; the foreground logic applies to the terminal device, not the shell's private fd. Confirm this in the reading pass.
Third risk: SIGCHLD timing. If a foreground job stops, the shell must be notified. With WUNTRACED, waitpid returns on stop. The shell's wait loop must distinguish stop from exit.
Fourth risk: interaction with S7's cooperative teardown. S7 used "close pipe fds, let children self-terminate." S8's job control still relies on this — a stopped job holds its pipe ends, so a pipeline containing a stopped stage can deadlock. Document this and test it.

8. Deliverables
Kernel: pgid/sid in TCB, setpgid/getpgrp/kill/sigaction/sigprocmask/tcsetpgrp/tcgetpgrp syscalls, stop/continue states
Shell: job table, &, jobs/fg/bg, %n specifiers, Ctrl+C/Ctrl+Z handling, terminal mode save/restore
Tests: kernel self-tests, host job-table tests, QEMU integration runner (test-shell-s8), Dell acceptance
Documentation: docs/plans/S8_PLAN.md, docs/roadmap/shell-s8.md, AGENTS.md status update
What Not to Miss
The five things that would make S8 wrong if omitted:
Process groups. Without them, Ctrl+C kills one process, not the pipeline.
Foreground group + tcsetpgrp. Without it, background jobs steal input.
Stop/continue with prompt return. Ctrl+Z must return control to the shell; fg must resume.
Terminal mode save/restore. Otherwise vi and less break the terminal for the shell.
Signal delivery at a clean point. A signal handler must not run in the middle of a kernel critical section.
If those five are right, the shell becomes a real job-control shell. Everything else — setsid, orphaned groups, SIGSTOP, TOSTOP — is refinement