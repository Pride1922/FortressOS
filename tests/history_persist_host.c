#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "types.h"
#include "terminal.h"
#include "syscall_abi.h"
#include "lineedit.h"
#include "history_persist.h"

/* Mock state */
static long s_mock_isatty = 1;
static int s_mock_save_calls = 0;
static bool s_mock_save_result = true;
static int s_mock_puts_calls = 0;
static char s_last_puts[256];

long call(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    (void)b; (void)c;
    if (nr == SYS_TERMCTL) {
        if (a == TERM_ISATTY) {
            return s_mock_isatty;
        }
        return 0;
    }
    return 0;
}

int puts(const char *s) {
    s_mock_puts_calls++;
    size_t len = strlen(s);
    if (len >= sizeof(s_last_puts)) len = sizeof(s_last_puts) - 1;
    memcpy(s_last_puts, s, len);
    s_last_puts[len] = '\0';
    return (int)len;
}

/* Mock history_save overriding the real one */
bool history_save(void) {
    s_mock_save_calls++;
    return s_mock_save_result;
}

int main(void) {
    printf("[TEST] Persistent History Auto-Flush Host Suite...\n");

    /* 1. Initial state verification */
    history_clear();
    assert(history_count() == 0);
    assert(!history_is_dirty());
    assert(history_get_counter() == 0);
    assert(!history_is_warned());

    /* 2. Counter behavior: after 4 commands no save, after 5 save, after 6 no save, after 10 save again */
    s_mock_isatty = 1;
    s_mock_save_calls = 0;
    s_mock_save_result = true;

    for (int i = 1; i <= 4; i++) {
        history_add("echo test");
        history_mark_dirty();
        history_autoflush_maybe();
        assert(history_get_counter() == (uint32_t)i);
        assert(s_mock_save_calls == 0);
        assert(history_is_dirty());
    }

    /* 5th command triggers save */
    history_add("echo cmd5");
    history_mark_dirty();
    history_autoflush_maybe();
    assert(s_mock_save_calls == 1);
    assert(history_get_counter() == 0);
    assert(!history_is_dirty());

    /* 6th command: no save */
    history_add("echo cmd6");
    history_mark_dirty();
    history_autoflush_maybe();
    assert(s_mock_save_calls == 1);
    assert(history_get_counter() == 1);
    assert(history_is_dirty());

    /* Commands 7, 8, 9: no save */
    for (int i = 7; i <= 9; i++) {
        history_add("echo more");
        history_mark_dirty();
        history_autoflush_maybe();
        assert(s_mock_save_calls == 1);
    }
    assert(history_get_counter() == 4);

    /* 10th command: save again */
    history_add("echo cmd10");
    history_mark_dirty();
    history_autoflush_maybe();
    assert(s_mock_save_calls == 2);
    assert(history_get_counter() == 0);
    assert(!history_is_dirty());
    printf("  [PASS] 5-command interval counter progression (4 no-save, 5 save, 6 no-save, 10 save)\n");

    /* 3. Failed commands count toward the interval */
    /* Commands that fail (syntax error, command not found, exit != 0) do not add to history
     * but history_autoflush_maybe() is still called in shell loop */
    history_mark_dirty(); /* simulated prior dirty state */
    for (int i = 1; i <= 4; i++) {
        /* Failed command runs: no history_add, but autoflush called */
        history_autoflush_maybe();
        assert(history_get_counter() == (uint32_t)i);
    }
    assert(s_mock_save_calls == 2);
    /* 5th failed command triggers save */
    history_autoflush_maybe();
    assert(s_mock_save_calls == 3);
    assert(history_get_counter() == 0);
    assert(!history_is_dirty());
    printf("  [PASS] Failed commands count toward the flush interval\n");

    /* 4. history clear sets dirty flag -> next flush writes empty history */
    history_clear();
    history_mark_dirty();
    assert(history_is_dirty());
    assert(history_count() == 0);
    for (int i = 1; i <= 5; i++) {
        history_autoflush_maybe();
    }
    assert(s_mock_save_calls == 4);
    assert(!history_is_dirty());
    assert(history_get_counter() == 0);
    printf("  [PASS] history clear marks dirty and saves on next flush\n");

    /* 5. history load resets dirty flag, counter, and warned flag */
    history_mark_dirty();
    for (int i = 0; i < 3; i++) history_autoflush_maybe();
    assert(history_is_dirty());
    assert(history_get_counter() == 3);

    /* In history_load, dirty is cleared and counter is reset to 0 */
    /* We simulate history_load state reset */
    history_load(); /* Will fail opening /mnt/.fortress/history, but resets state */
    assert(!history_is_dirty());
    assert(history_get_counter() == 0);
    assert(!history_is_warned());

    /* Running commands without dirty flag does NOT trigger save */
    for (int i = 1; i <= 5; i++) {
        history_autoflush_maybe();
    }
    assert(s_mock_save_calls == 4); /* still 4, no re-save because not dirty */
    assert(history_get_counter() == 0);
    printf("  [PASS] history load clears dirty flag, resets counter to 0, and avoids immediate save\n");

    /* 6. Failed write does not crash, does not clear dirty flag, warns once per session */
    s_mock_save_result = false;
    s_mock_puts_calls = 0;
    history_mark_dirty();

    for (int i = 1; i <= 5; i++) {
        history_autoflush_maybe();
    }
    assert(s_mock_save_calls == 5);
    assert(history_is_dirty()); /* NOT cleared on failure */
    assert(history_is_warned()); /* Warned flag is set */
    assert(s_mock_puts_calls == 1);
    assert(strstr(s_last_puts, "history:") != NULL);

    /* Subsequent 5 commands fail again, but warning is suppressed */
    int puts_before = s_mock_puts_calls;
    for (int i = 1; i <= 5; i++) {
        history_autoflush_maybe();
    }
    assert(s_mock_save_calls == 6);
    assert(history_is_dirty());
    assert(s_mock_puts_calls == puts_before); /* No new warning emitted */
    printf("  [PASS] Failed write preserves dirty flag and suppresses repeated warnings\n");

    /* 7. Script mode (echo foo | shell): no auto-flush */
    s_mock_isatty = 0; /* Stdin is a pipe/script, not a terminal */
    s_mock_save_result = true;
    int saves_before = s_mock_save_calls;
    history_mark_dirty();

    for (int i = 0; i < 20; i++) {
        history_autoflush_maybe();
    }
    assert(s_mock_save_calls == saves_before); /* Zero auto-saves performed in non-interactive mode */
    assert(history_get_counter() == 0);
    printf("  [PASS] Script/pipe mode bypasses auto-flush entirely\n");

    printf(">>> ALL PERSISTENT HISTORY AUTO-FLUSH HOST TESTS PASSED (100%%%%) <<<\n");
    return 0;
}
