#include "builtins.h"
#include "io.h"

static const struct { const char *name, *help; enum builtin id; } commands[] = {
    {"help", "Show commands [name]", CMD_HELP},
    {"cd", "Change working directory [path | -]", CMD_CD},
    {"pwd", "Print working directory", CMD_PWD},
    {"type", "Display information about command type", CMD_TYPE},
    {"command", "Execute a simple command or builtin", CMD_COMMAND},
    {"true", "Return successful exit status 0", CMD_TRUE},
    {"false", "Return failure exit status 1", CMD_FALSE},
    {"ls", "List files [path]", CMD_LS},
    {"cat", "Read a text file", CMD_CAT},
    {"edit", "Open line-oriented file editor", CMD_EDIT},
    {"mkdir", "Create directory", CMD_MKDIR},
    {"rm", "Remove file or empty directory", CMD_RM},
    {"mv", "Rename file or directory", CMD_MV},
    {"sync", "Flush writable storage", CMD_SYNC},
    {"echo", "Print text (supports $?)", CMD_ECHO},
    {"run", "Launch /path [args] (compat wrapper)", CMD_RUN},
    {"layout", "Keyboard: us | azerty", CMD_LAYOUT},
    {"reboot", "Restart system", CMD_REBOOT},
    {"shutdown", "Power off system", CMD_SHUTDOWN},
    {"poweroff", "Alias for shutdown", CMD_POWEROFF},
    {"exit", "Exit shell [status]", CMD_EXIT},
    {"dmesg", "Print/save kernel log [path]", CMD_DMESG},
    {"history", "History [clear | save | load]", CMD_HISTORY},
    {"prompt", "Configure prompt format [default | cwd | <template>]", CMD_PROMPT},
    {"terminal", "Select local | serial | mirror | plain output", CMD_TERMINAL},
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
         "Working Dir: cd, cd -, pwd, process-inherited cwd.\n"
         "Discovery: direct execution (/bin/hello, ./tool, hello searches /bin).\n"
         "History: RAM history bounded 1000/256K; persistent at /mnt/.fortress/history.\n");
}
