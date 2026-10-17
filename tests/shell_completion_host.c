#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "complete.h"
#include "io.h"

/* Directory syscall adapter; completion itself is production code. */
static const char *directory;
static unsigned index_in_dir;
bool equal(const char *a, const char *b) { return strcmp(a, b) == 0; }
long call(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    (void)c;
    if (nr == SYS_OPEN) {
        directory = (const char *)a;
        index_in_dir = 0;
        return 3;
    }
    if (nr == SYS_CLOSE) return 0;
    assert(nr == SYS_READDIR && a == 3);
    const char *name = NULL;
    unsigned type = VFS_FILE;
    if (!strcmp(directory, "/")) {
        if (index_in_dir == 0) { name = "sbin"; type = VFS_DIRECTORY; }
        else if (index_in_dir == 1) { name = "bin"; type = VFS_DIRECTORY; }
    } else if (!strcmp(directory, "/sbin")) {
        if (index_in_dir == 0) name = "reboot";
        else if (index_in_dir == 1) name = "shutdown";
    } else if (!strcmp(directory, ".")) {
        if (index_in_dir == 0) name = "tool";
    }
    if (!name) return 0;
    vfs_dirent_t *entry = (vfs_dirent_t *)b;
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->name, name, strlen(name) + 1); entry->type = type;
    index_in_dir++;
    return 1;
}
static line_editor_t editor;
static void check(const char *input, const char *expected) {
    lineedit_init(&editor);
    memcpy(editor.text, input, strlen(input) + 1);
    editor.cursor = editor.len = strlen(input);
    assert(shell_do_completion(&editor) == 1);
    assert(!strcmp(editor.text, expected));
    assert(editor.cursor == strlen(expected));
}
int main(void) {
    check("/sb", "/sbin/");
    check("/sbin/reb", "/sbin/reboot ");
    check("./to", "./tool ");
    check("true; /sb", "true; /sbin/");
    check("sudo /sb", "sudo /sbin/");
    check("cd /sb", "cd /sbin/");
    check("type /sbin/reb", "type /sbin/reboot ");
    check("reb", "reboot ");
    check("ech", "echo ");
    puts("PASS command/argument absolute and relative path completion, directories and bare names");
    return 0;
}
