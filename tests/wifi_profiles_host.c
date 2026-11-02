/* Actual CLI profile save/load/boot-auto path; filesystem, radio and child
 * syscalls are mocked. No physical persistence or RF acceptance is claimed. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
static long fixture_call(long, uintptr_t, uintptr_t, uintptr_t);
#define WIFI_SYSCALL(n,a,b,c) fixture_call(n,(uintptr_t)(a),(uintptr_t)(b),(uintptr_t)(c))
#include "../user/wifi.c"
static void fixture_copy(char *dst, const char *src) { memcpy(dst, src, strlen(src) + 1); }
static char disk[8192], temporary[8192];
static size_t temporary_len, read_offset;
static unsigned connects, spawns, syncs;
static unsigned radio_state = WIFI_STATUS_IDLE;
static bool short_write, fail_write, fail_rename, volatile_filesystem;
static long fixture_call(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    if (nr == SYS_OPEN) {
        if (!strcmp((char *)a, "/etc/network")) { assert(b==0); return 5; }
        if (!strcmp((char *)a, "/etc/network/wlan.conf")) {
            assert(b == 0); read_offset = 0; return 3;
        }
        assert(!strcmp((char *)a, "/etc/network/.wlan-cli.new"));
        assert(b == 0x800c1 && c == 0600);
        temporary_len = 0; return 4;
    }
    if (nr == SYS_READ) {
        assert(a == 3); size_t n = strlen(disk) - read_offset;
        if (n > c) n = c;
        if (n > 11) n = 11;
        memcpy((void *)b, disk + read_offset, n); read_offset += n; return (long)n;
    }
    if (nr == SYS_WRITE) {
        if (a < 3) return (long)c;
        assert(a == 4);
        if (fail_write) return -1;
        size_t n = short_write && c > 7 ? 7 : c;
        assert(temporary_len + n < sizeof(temporary));
        memcpy(temporary + temporary_len, (void *)b, n);
        temporary_len += n; temporary[temporary_len] = 0; return (long)n;
    }
    if (nr == SYS_FCHMOD) { assert(a == 4 && b == 0600); return 0; }
    if (nr == SYS_FSYNC) { assert(a == 4 || a == 5); if (a==5) syncs++; return volatile_filesystem ? SYSCALL_EOPNOTSUPP : 0; }
    if (nr == SYS_CLOSE || nr == SYS_MKDIR || nr == SYS_UNLINK || nr == SYS_NANOSLEEP) return 0;
    if (nr == SYS_SYNC) { assert(!"profile save must not flush unrelated mounts"); return SYSCALL_EIO; }
    if (nr == SYS_RENAME_REPLACE) {
        assert(!strcmp((char *)b, "/etc/network/wlan.conf"));
        if (fail_rename) return -1;
        fixture_copy(disk, temporary); return 0;
    }
    if (nr == SYS_WIFI) {
        if (a == WIFI_CMD_SCAN) {
            wifi_scan_result_t *scan = (void *)b;
            if (scan->max_entries) {
                scan->count = 1; fixture_copy(scan->entries[0].ssid, "Halloween");
                scan->entries[0].rssi = -45;
            }
            return 0;
        }
        if (a == WIFI_CMD_CONNECT) {
            wifi_connect_req_t *req = (void *)b;
            assert(!strcmp(req->ssid, "Halloween"));
            assert(!strcmp(req->passphrase, "SavedSecret2026")); connects++; radio_state=WIFI_STATUS_CONNECTED; return 0;
        }
        assert(a == WIFI_CMD_STATUS);
        wifi_status_t *st = (void *)b; memset(st, 0, sizeof(*st));
        st->state = radio_state; st->link_online = radio_state == WIFI_STATUS_CONNECTED; return 0;
    }
    if (nr == SYS_SPAWN_EXT) {
        assert(!strcmp((char *)a, "/sbin/dhcpc")); spawns++; return 7;
    }
    if (nr == SYS_WAIT) { *(uint64_t *)b = 0; return 0; }
    assert(!"Unexpected profile effect"); return -1;
}
int gzip_decode_callbacks(gzip_read_fn r, gzip_write_fn w, void *c) {
    (void)r; (void)w; (void)c; return -1;
}
int main(void) {
    short_write = true;
    assert(!do_save("Halloween", "SavedSecret2026", NULL, NULL, NULL));
    assert(strstr(disk, "psk=\"SavedSecret2026\""));
    assert(syncs == 1);
    volatile_filesystem=true;
    assert(!do_save("Halloween", "SavedSecret2026", NULL, NULL, NULL));
    assert(strstr(disk,"Halloween"));
    volatile_filesystem=false;
    /* Reboot: discard all process state, then execute the boot service CLI. */
    memset(s_saved_nets, 0, sizeof(s_saved_nets)); s_saved_count = 0;
    s_fw_loaded = true; /* Radio firmware mocked ready. */
    char *argv[] = {"wifi", "auto", NULL};
    assert(!wifi_main(2, argv));
    assert(connects == 1 && spawns == 1);
    assert(load_saved_networks() == 1);
    assert(!strcmp(s_saved_nets[0].ssid, "Halloween"));
    assert(!strcmp(s_saved_nets[0].psk, "SavedSecret2026"));
    /* The same save runs again next boot, without wiping its own inputs. */
    radio_state=WIFI_STATUS_IDLE; /* A second real boot has no radio association. */
    assert(!wifi_main(2, argv)); assert(connects == 2 && spawns == 2);
    /* A supervisor/DHCP retry on a live link must not tear it down. */
    assert(!wifi_main(2, argv)); assert(connects == 2 && spawns == 2);
    for (unsigned state=WIFI_STATUS_AUTHENTICATING;state<=WIFI_STATUS_HANDSHAKE;++state) {
        radio_state=state;
        assert(wifi_main(2, argv)==1); assert(connects == 2 && spawns == 2);
    }
    radio_state=WIFI_STATUS_CONNECTED;
    char before[8192]; fixture_copy(before, disk);
    fail_write = true;
    assert(do_save("Halloween", "ReplacementSecret", NULL, NULL, NULL));
    assert(!strcmp(before, disk)); fail_write = false;
    fail_rename = true;
    assert(do_save("Halloween", "ReplacementSecret", NULL, NULL, NULL));
    assert(!strcmp(before, disk)); fail_rename = false;
    /* All sixteen maximum-size profiles must round trip without truncation. */
    for (unsigned i = 0; i < 15; ++i) {
        char ssid[33]; snprintf(ssid, sizeof(ssid), "LongSavedNetworkNameNumber%06u", i);
        assert(!do_save(ssid, "LongPasswordForFullCapacityRegression2026", "192.168.100.100/24", "192.168.100.1", "192.168.100.2"));
    }
    assert(strlen(disk) > 2048 && load_saved_networks() == 16);
    puts("Profile persistence PASS: boot auto-connect twice, credentials intact, short writes, failed-write/rename preservation, 16-profile round trip");
}
