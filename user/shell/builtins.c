#include "builtins.h"
#include "io.h"

static const struct {
    const char *name, *help;
    enum builtin id;
    bool child_safe; /* May run as a /bin/sh-builtin pipeline stage. Default: false. */
} commands[] = {
    {"help",     "Show commands [name]",                                    CMD_HELP,     true},
    {"clear",    "Clear the screen and move the cursor home",               CMD_CLEAR,    true},
    {"cd",       "Change working directory [path | -]",                    CMD_CD,       false},
    {"pwd",      "Print working directory",                                CMD_PWD,      true},
    {"type",     "Display information about command type",                 CMD_TYPE,     true},
    {"command",  "Execute a simple command or builtin",                    CMD_COMMAND,  false},
    {"true",     "Return successful exit status 0",                        CMD_TRUE,     true},
    {"false",    "Return failure exit status 1",                           CMD_FALSE,    true},
    {"ls",       "List files [-l] [path] (named owners)",                   CMD_LS,       true},
    {"umask",    "Print or set creation mask [octal]",                      CMD_UMASK,    false},
    {"view",     "View text: replace non-printable bytes with dots and finish the line", CMD_VIEW, true},
    {"edit",     "Open line-oriented file editor",                         CMD_EDIT,     false},
    {"mkdir",    "Create directory",                                       CMD_MKDIR,    false},
    {"rm",       "Remove file or empty directory",                         CMD_RM,       false},
    {"mv",       "Rename file or directory",                               CMD_MV,       false},
    {"sync",     "Flush writable storage",                                 CMD_SYNC,     false},
    {"echo",     "Print text (supports $?)",                               CMD_ECHO,     true},
    {"printf",   "Format and print data without trailing newline",         CMD_PRINTF,   true},
    {"run",      "Launch /path [args] (compat wrapper)",                   CMD_RUN,      false},
    {"layout",   "Keyboard: us | azerty",                                  CMD_LAYOUT,   false},
    {"reboot",   "Restart system",                                         CMD_REBOOT,   false},
    {"shutdown", "Power off system",                                       CMD_SHUTDOWN, false},
    {"poweroff", "Alias for shutdown",                                     CMD_POWEROFF, false},
    {"exit",     "Exit shell [status]",                                    CMD_EXIT,     false},
    {"dmesg",    "Print/save kernel log [-n N | tail [N]] [path]",         CMD_DMESG,    true},
    {"history",  "History [clear | save | load]",                          CMD_HISTORY,  false},
    {"prompt",   "Configure prompt format [default | cwd | <template>]",  CMD_PROMPT,   false},
    {"terminal", "Select local | serial | mirror | plain output",          CMD_TERMINAL, false},
    {"set",      "Print all shell variables",                              CMD_SET,      false},
    {"unset",    "Unset shell variables [name...]",                        CMD_UNSET,    false},
    {"export",   "Set/export environment variables [name[=val]...]",       CMD_EXPORT,   false},
    {"env",      "Print exported environment variables",                   CMD_ENV,      true},
    {"alias",    "Define or display aliases [name[='val']...]",            CMD_ALIAS,    false},
    {"unalias",  "Remove aliases [name...]",                               CMD_UNALIAS,  false},
    {"jobs", "List owned jobs", CMD_JOBS, false},
    {"fg", "Foreground job [%n | %+ | %-] (default %+)", CMD_FG, false},
    {"bg", "Resume job in background [%n | %+ | %-]", CMD_BG, false},
    {"kill", "Signal job: kill %n [signal name or number] (default TERM)", CMD_KILL, false},
    {"version",  "Print FortressOS version and build info",                CMD_VERSION,  true},
};

size_t builtin_count(void) {
    return sizeof(commands) / sizeof(commands[0]);
}

const char *builtin_name(size_t index) {
    if (index < builtin_count()) return commands[index].name;
    return "";
}

enum builtin builtin_find(const char *name) {
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (equal(name, commands[i].name)) return commands[i].id;
    }
    return CMD_UNKNOWN;
}

bool builtin_is_child_safe(const char *name) {
    if (!name || !*name) return false;
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (equal(name, commands[i].name)) return commands[i].child_safe;
    }
    return false;
}

void builtin_help(const char *topic) {
    if (topic && *topic) {
        for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
            if (equal(topic, commands[i].name)) {
                puts(commands[i].name);
                puts(" - ");
                puts(commands[i].help);
                puts("\n");
                return;
            }
        }
        if (equal(topic, "tracert")) {
            puts("tracert - Alias for traceroute\n");
            return;
        }
        if (equal(topic, "traceroute")) {
            puts("traceroute - Trace network route to IPv4 host\n");
            return;
        }
        puts("help: no help topic for '");
        puts(topic);
        puts("'\n");
        return;
    }

    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        puts(commands[i].name);
        puts("  ");
        puts(commands[i].help);
        puts("\n");
    }
    puts("Editing: Tab complete, Arrows/Home/End/Del, Up/Down history, Ctrl+R search.\n"
         "Shortcuts: Ctrl+A/E/W/U/K/Y/L, Ctrl+C cancels input, Ctrl+D empty exits.\n"
         "Syntax: Quotes ('...'/\"...\"), escapes (\\), chaining (;, &&, ||), negation (!).\n"
         "Pipelines: echo printf pwd true false env help version clear ls view type dmesg run as pipeline stages.\n"
         "  Other builtins (cd, export, alias, ...) are not available in pipeline stages.\n"
         "  In pipeline context, 'type' reports only pipeline-stage builtins (not cd etc.).\n"
         "Stream tools: cat, head, tail, wc (external; use TOOL --help). cat preserves bytes; view sanitizes text.\n"
         "Aliases: tracert (alias for traceroute).\n"
         "Working Dir: cd, cd -, pwd, process-inherited cwd.\n"
         "Discovery: direct execution (/bin/hello, ./tool, hello searches /bin).\n"
         "History: RAM history bounded 1000/256K; persistent at /mnt/.fortress/history.\n");
}
