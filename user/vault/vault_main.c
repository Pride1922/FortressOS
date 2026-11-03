/* user/vault/vault_main.c - FortressOS Credential Vault CLI (/sbin/vault)
 *
 * Commands:
 *   vault init [-f PATH] [-p PASS]
 *   vault set <KEY> [-f PATH] [-p PASS] [-s SECRET | --stdin] [--type TYPE] [--meta META]
 *   vault get <KEY> [-f PATH] [-p PASS] [--raw]
 *   vault list [-f PATH] [-p PASS]
 *   vault delete <KEY> [-f PATH] [-p PASS]
 *   vault change-password [-f PATH] [-p OLD_PASS] [-n NEW_PASS]
 */

#include "vault.h"
#include "../../src/include/syscall_abi.h"
#include "../../src/include/terminal.h"
#include "../../src/include/crypto.h"
#include "../../src/fs/vfs.h"

#ifdef TOOL_HOST_TEST
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
static inline int tool_write(const char *tool, const void *str, size_t len) {
    (void)tool;
    return (int)write(1, str, len);
}
static inline int tool_error(const char *tool, const char *msg, const char *op) {
    fprintf(stderr, "%s: %s %s\n", tool, msg, op ? op : "");
    return 1;
}
#else
#include "../../user/tools/common.h"
#include "../../user/tools/userdb.h"
static inline void *vault_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}
static inline void *vault_memset(void *dst, int val, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)val;
    return dst;
}
static inline int vault_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
static inline size_t vault_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
#define memcpy vault_memcpy
#define memset vault_memset
#define strcmp vault_strcmp
#define strlen vault_strlen
#endif

#define DEFAULT_SYSTEM_VAULT "/etc/vault.db"

static vault_db_t s_cli_db;
#ifndef TOOL_HOST_TEST
static userdb_t s_admin_userdb;
#endif

static void out_str(const char *s) {
    tool_write("vault", s, strlen(s));
}

static void out_err(const char *msg, const char *op) {
    tool_error("vault", msg, op);
}

static bool read_hidden_passphrase(const char *prompt, char *buf, size_t max_len) {
    out_str(prompt);

    size_t at = 0;
    while (at + 1 < max_len) {
        char ch = 0;
        long r = tool_syscall(SYS_READ, 0, (uintptr_t)&ch, 1);
        if (r <= 0) break;
        if (ch == '\n' || ch == '\r') {
            break;
        }
        if (ch == 8 || ch == 127) { /* Backspace */
            if (at > 0) at--;
            continue;
        }
        if (ch >= 32 && ch <= 126) {
            buf[at++] = ch;
        }
    }
    buf[at] = '\0';
    out_str("\n");
    return at > 0;
}

static int prompt_passphrase(char *buf, size_t max_len) {
    if (!read_hidden_passphrase("Vault Passphrase: ", buf, max_len)) {
        out_err("passphrase cannot be empty", NULL);
        return -1;
    }
    return 0;
}

static const char *type_name(uint8_t t) {
    switch (t) {
        case VAULT_TYPE_PASSWORD: return "password";
        case VAULT_TYPE_SSH_KEY:  return "ssh_key";
        case VAULT_TYPE_TOKEN:    return "token";
        default:                  return "generic";
    }
}

static uint8_t parse_type(const char *s) {
    if (!s) return VAULT_TYPE_PASSWORD;
    if (strcmp(s, "ssh_key") == 0 || strcmp(s, "key") == 0) return VAULT_TYPE_SSH_KEY;
    if (strcmp(s, "token") == 0) return VAULT_TYPE_TOKEN;
    if (strcmp(s, "generic") == 0) return VAULT_TYPE_GENERIC;
    return VAULT_TYPE_PASSWORD;
}

static void usage(void) {
    out_str("FortressOS Credential Vault\n"
            "Usage: vault <command> [options]\n\n"
            "Commands:\n"
            "  init    [-f PATH] [-p PASS]                Create a fresh encrypted vault\n"
            "  set     <KEY> [-s VAL | --stdin] [-p PASS] Store or update a secret\n"
            "  get     <KEY> [-p PASS] [--raw]            Retrieve a stored secret\n"
            "  list    [-f PATH] [-p PASS]                List all stored keys\n"
            "  delete  <KEY> [-f PATH] [-p PASS]          Remove a stored secret\n"
            "  rekey   [-p OLD] [-n NEW]                  Change the master passphrase\n\n"
            "Options:\n"
            "  -f PATH        Path to vault file (default: /etc/vault.db)\n"
            "  -p PASS        Master passphrase (prompts if omitted)\n"
            "  -s SECRET      Secret string to store\n"
            "  --stdin        Read full secret from standard input\n"
            "  --type TYPE    Secret type (password, ssh_key, token, generic)\n"
            "  --meta META    Descriptive metadata\n"
            "  --raw          Output raw secret without trailing newline\n");
}

int vault_main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage();
        return 0;
    }

    const char *cmd = argv[1];
    const char *vault_path = DEFAULT_SYSTEM_VAULT;
    const char *key = NULL;
    const char *pass = NULL;
    const char *new_pass = NULL;
    const char *secret_val = NULL;
    const char *meta = NULL;
    uint8_t secret_type = VAULT_TYPE_PASSWORD;
    bool from_stdin = false;
    bool raw_output = false;

    int idx = 2;
    if (strcmp(cmd, "set") == 0 || strcmp(cmd, "get") == 0 || strcmp(cmd, "delete") == 0) {
        if (argc < 3 || argv[2][0] == '-') {
            out_err("missing required <KEY> argument for", cmd);
            return 1;
        }
        key = argv[2];
        idx = 3;
    }

    while (idx < argc) {
        if (strcmp(argv[idx], "-f") == 0 && idx + 1 < argc) {
            vault_path = argv[++idx];
        } else if (strcmp(argv[idx], "-p") == 0 && idx + 1 < argc) {
            pass = argv[++idx];
        } else if (strcmp(argv[idx], "-n") == 0 && idx + 1 < argc) {
            new_pass = argv[++idx];
        } else if (strcmp(argv[idx], "-s") == 0 && idx + 1 < argc) {
            secret_val = argv[++idx];
        } else if (strcmp(argv[idx], "--type") == 0 && idx + 1 < argc) {
            secret_type = parse_type(argv[++idx]);
        } else if (strcmp(argv[idx], "--meta") == 0 && idx + 1 < argc) {
            meta = argv[++idx];
        } else if (strcmp(argv[idx], "--stdin") == 0) {
            from_stdin = true;
        } else if (strcmp(argv[idx], "--raw") == 0) {
            raw_output = true;
        }
        idx++;
    }

    char pass_buf[128];
#ifndef TOOL_HOST_TEST
    /* Enforce administrative authorization for system vault access */
    if (vault_path[0] == '/' && vault_path[1] == 'e' && vault_path[2] == 't' && vault_path[3] == 'c') {
        uint32_t ruid = 0, euid = 0, suid = 0;
        if (tool_syscall(SYS_GETRESUID, (uintptr_t)&ruid, (uintptr_t)&euid, (uintptr_t)&suid) == 0) {
            if (ruid != 0) {
                bool is_admin = false;
                if (db_load(&s_admin_userdb, false)) {
                    const db_user_t *u = db_user_id(&s_admin_userdb, ruid);
                    if (u) {
                        uint32_t gids[16]; unsigned gcount = 0;
                        const db_group_t *w = NULL;
                        for (unsigned i = 0; i < s_admin_userdb.ngroups; i++) {
                            if (tool_equal(s_admin_userdb.groups[i].name, "wheel")) {
                                w = &s_admin_userdb.groups[i];
                                break;
                            }
                        }
                        if (w && db_groups(&s_admin_userdb, u, gids, &gcount)) {
                            for (unsigned i = 0; i < gcount; i++) {
                                if (gids[i] == w->gid) { is_admin = true; break; }
                            }
                        }
                    }
                }
                if (!is_admin) {
                    out_err("permission denied: only administrators can access system vault", NULL);
                    return 1;
                }
            }
        }
    }
#endif

    if (!pass) {
        if (prompt_passphrase(pass_buf, sizeof(pass_buf)) != 0) {
            return 1;
        }
        pass = pass_buf;
    }

    int status = 0;

    if (strcmp(cmd, "init") == 0) {
        vault_db_init_empty(&s_cli_db, VAULT_DEFAULT_ITERATIONS);
        int rc = vault_file_save(vault_path, pass, &s_cli_db);
        vault_db_clear(&s_cli_db);
        if (rc == VAULT_ERR_OK) {
            out_str("Vault initialized at: ");
            out_str(vault_path);
            out_str("\n");
        } else {
            out_err("failed to initialize vault", vault_path);
            status = 1;
        }
    } else if (strcmp(cmd, "set") == 0) {
        static uint8_t secret_buf[VAULT_MAX_SECRET_LEN];
        size_t slen = 0;

        if (from_stdin) {
            while (slen < sizeof(secret_buf)) {
                long r = tool_syscall(SYS_READ, 0, (uintptr_t)(secret_buf + slen),
                                      sizeof(secret_buf) - slen);
                if (r <= 0) break;
                slen += (size_t)r;
            }
        } else if (secret_val) {
            slen = strlen(secret_val);
            if (slen > sizeof(secret_buf)) slen = sizeof(secret_buf);
            memcpy(secret_buf, secret_val, slen);
        } else {
            char pbuf[512];
            if (!read_hidden_passphrase("Secret: ", pbuf, sizeof(pbuf))) {
                out_err("empty secret rejected", NULL);
                crypto_wipe(pass_buf, sizeof(pass_buf));
                return 1;
            }
            slen = strlen(pbuf);
            memcpy(secret_buf, pbuf, slen);
            crypto_wipe(pbuf, sizeof(pbuf));
        }

        int rc = vault_set_secret(vault_path, pass, key, secret_type, secret_buf, slen, meta);
        crypto_wipe(secret_buf, sizeof(secret_buf));

        if (rc == VAULT_ERR_OK) {
            out_str("Secret stored: ");
            out_str(key);
            out_str("\n");
        } else if (rc == VAULT_ERR_AUTH) {
            out_err("authentication failed (incorrect master passphrase)", NULL);
            status = 1;
        } else {
            out_err("failed to store secret in vault", vault_path);
            status = 1;
        }
    } else if (strcmp(cmd, "get") == 0) {
        static uint8_t secret_buf[VAULT_MAX_SECRET_LEN + 1];
        size_t slen = 0;

        int rc = vault_get_secret(vault_path, pass, key, secret_buf, sizeof(secret_buf) - 1, &slen);
        if (rc == VAULT_ERR_OK) {
            tool_write("vault", secret_buf, slen);
            if (!raw_output && (slen == 0 || secret_buf[slen - 1] != '\n')) {
                out_str("\n");
            }
            crypto_wipe(secret_buf, sizeof(secret_buf));
        } else if (rc == VAULT_ERR_NOTFOUND) {
            out_err("key not found in vault:", key);
            status = 1;
        } else if (rc == VAULT_ERR_AUTH) {
            out_err("authentication failed (incorrect master passphrase)", NULL);
            status = 1;
        } else {
            out_err("failed to access vault", vault_path);
            status = 1;
        }
    } else if (strcmp(cmd, "list") == 0) {
        int rc = vault_file_load(vault_path, pass, &s_cli_db);
        if (rc == VAULT_ERR_OK) {
            out_str("Stored Keys (");
#ifdef TOOL_HOST_TEST
            printf("%zu", s_cli_db.count);
#else
            char cnt_buf[16];
            size_t clen = tool_format_u64(cnt_buf, s_cli_db.count);
            tool_write("vault", cnt_buf, clen);
#endif
            out_str(" entries):\n");

            for (size_t i = 0; i < s_cli_db.count; i++) {
                out_str("  - ");
                out_str(s_cli_db.records[i].key);
                out_str(" [");
                out_str(type_name(s_cli_db.records[i].secret_type));
                out_str("]");
                if (s_cli_db.records[i].meta[0]) {
                    out_str(" (");
                    out_str(s_cli_db.records[i].meta);
                    out_str(")");
                }
                out_str("\n");
            }
            vault_db_clear(&s_cli_db);
        } else if (rc == VAULT_ERR_AUTH) {
            out_err("authentication failed (incorrect master passphrase)", NULL);
            status = 1;
        } else {
            out_err("failed to load vault", vault_path);
            status = 1;
        }
    } else if (strcmp(cmd, "delete") == 0) {
        int rc = vault_file_load(vault_path, pass, &s_cli_db);
        if (rc == VAULT_ERR_OK) {
            rc = vault_db_delete(&s_cli_db, key);
            if (rc == VAULT_ERR_OK) {
                rc = vault_file_save(vault_path, pass, &s_cli_db);
                if (rc == VAULT_ERR_OK) {
                    out_str("Deleted key: ");
                    out_str(key);
                    out_str("\n");
                } else {
                    out_err("failed to update vault", vault_path);
                    status = 1;
                }
            } else {
                out_err("key not found:", key);
                status = 1;
            }
            vault_db_clear(&s_cli_db);
        } else if (rc == VAULT_ERR_AUTH) {
            out_err("authentication failed (incorrect master passphrase)", NULL);
            status = 1;
        } else {
            out_err("failed to load vault", vault_path);
            status = 1;
        }
    } else if (strcmp(cmd, "rekey") == 0) {
        char new_buf[128];
        if (!new_pass) {
            if (!read_hidden_passphrase("New Master Passphrase: ", new_buf, sizeof(new_buf))) {
                out_err("empty new passphrase rejected", NULL);
                crypto_wipe(pass_buf, sizeof(pass_buf));
                return 1;
            }
            new_pass = new_buf;
        }

        int rc = vault_file_load(vault_path, pass, &s_cli_db);
        if (rc == VAULT_ERR_OK) {
            rc = vault_file_save(vault_path, new_pass, &s_cli_db);
            vault_db_clear(&s_cli_db);
            if (rc == VAULT_ERR_OK) {
                out_str("Master passphrase updated successfully.\n");
            } else {
                out_err("failed to save re-keyed vault", vault_path);
                status = 1;
            }
        } else if (rc == VAULT_ERR_AUTH) {
            out_err("authentication failed with current passphrase", NULL);
            status = 1;
        } else {
            out_err("failed to load vault", vault_path);
            status = 1;
        }
        crypto_wipe(new_buf, sizeof(new_buf));
    } else {
        out_err("unknown command:", cmd);
        usage();
        status = 1;
    }

    crypto_wipe(pass_buf, sizeof(pass_buf));
    return status;
}
