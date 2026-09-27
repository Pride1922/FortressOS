/*
 * /bin/sh-builtin — Builtin pipeline stage runner.
 *
 * This ELF is spawned by the shell for each child-safe builtin in a pipeline.
 * It dispatches exactly one allowed command and exits with its result.
 *
 * It never:
 *   - Initializes the shell UI, history, prompt, alias table or terminal.
 *   - Parses or re-expands any command text.
 *   - Opens the retained terminal fd (fd 31).
 *   - Enters a loop or reads interactive input.
 *
 * argv[0] is the builtin name (e.g. "echo"), passed by the parent pipeline.
 * Direct invocation as "/bin/sh-builtin NAME [ARG...]" is also accepted; in
 * that case argv[0] is "sh-builtin" and argv[1] is the command name, so we
 * normalize by shifting argv by 1.
 *
 * Exit codes:
 *   0   success
 *   1   error
 *   2   unknown or forbidden command (reject code; not re-used for EPIPE)
 *   141 stdout EPIPE (silent; no diagnostic)
 */
#include "types.h"
#include "vfs.h"
#include "syscall_abi.h"
#include "shell/io.h"
#include "shell/builtin_exec.h"
#include "shell/builtins.h"

/*
 * sh_builtin_main — called from sh_builtin_start.asm with the kernel-supplied
 * argc/argv/envp.
 *
 * The System V freestanding ABI passes these in RDI/RSI/RDX; the C compiler
 * maps them to argc/argv/envp automatically when declared as below.
 */
int sh_builtin_main(int argc, const char *const *argv, const char *const *envp) {
    if (argc < 1 || !argv || !argv[0] || !argv[0][0]) {
        (void)write_bytes_fd(2, "sh-builtin: missing command\n",
                             length("sh-builtin: missing command\n"));
        return 2;
    }

    /*
     * Normalize direct invocation: "/bin/sh-builtin NAME [ARG...]"
     * When argv[0] is "sh-builtin" (or contains '/'), the builtin name is
     * argv[1].  When argv[0] is already a builtin name (pipeline path), use it
     * directly.
     */
    const char *cmd_name = argv[0];
    /* Check for "sh-builtin" or path ending in "sh-builtin" */
    bool is_wrapper = false;
    {
        const char *p = cmd_name;
        const char *last_slash = NULL;
        for (const char *c = p; *c; c++) if (*c == '/') last_slash = c;
        const char *base = last_slash ? last_slash + 1 : p;
        /* Compare base name against "sh-builtin" */
        const char *ref = "sh-builtin";
        size_t i = 0;
        while (base[i] && ref[i] && base[i] == ref[i]) i++;
        if (!base[i] && !ref[i]) is_wrapper = true;
    }

    if (is_wrapper) {
        /* Direct invocation: shift argv past "sh-builtin" */
        if (argc < 2 || !argv[1] || !argv[1][0]) {
            (void)write_bytes_fd(2, "sh-builtin: missing command\n",
                                 length("sh-builtin: missing command\n"));
            return 2;
        }
        argc--;
        argv++;
    }

    /* Build context from envp (envp may be NULL in degenerate cases). */
    builtin_ctx_t ctx = builtin_ctx_from_envp(envp);

    /* Dispatch; builtin_exec() rejects non-allowlist names with status 2. */
    return builtin_exec(argc, argv, &ctx);
}
