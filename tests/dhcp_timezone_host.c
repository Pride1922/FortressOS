/* Exercise actual DHCP discovery publication and shared runtime resolver. */
#include "syscall_abi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static long mocked(long, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
#define DHCPC_TEST_CALL mocked
#include "../user/dhcpc.c"

static char published[256], pending[256];
static uint8_t zone[16384];
static size_t zone_len, position, pending_len;
static uint8_t database[1024 * 1024];
static size_t database_len;
static bool packed_mode;
static const uint8_t *read_source;
static size_t read_length;
static bool write_failure, rename_failure, readonly_failure;
static long net_result;
static uint8_t response[576];
static size_t response_size;
static bool wire_discovery = true;
static void copy(char *dst, const char *src) { memcpy(dst, src, strlen(src) + 1); }
static long mocked(long nr, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e,
                   uintptr_t f) {
    (void)c;
    (void)d;
    (void)e;
    (void)f;
    if (nr == SYS_CLOCK_GETTIME) {
        *(timespec_t *)b = (timespec_t){1782864000, 0};
        return 0;
    }
    if (nr == SYS_SOCKET)
        return 8;
    if (nr == SYS_BIND)
        return 0;
    if (nr == SYS_SYSINFO) {
        memset((void *)a, 0, sizeof(sysinfo_t));
        return 0;
    }
    if (nr == SYS_GETRANDOM) {
        memset((void *)a, 7, b);
        return b;
    }
    if (nr == SYS_SENDTO) {
        assert(a == 8 && c >= 240);
        memset(response, 0, sizeof(response));
        memcpy(response, (const void *)b, 240);
        dhcp_packet_hdr_t *header = (dhcp_packet_hdr_t *)response;
        header->op = DHCP_BOOTREPLY;
        header->yiaddr = 0x0f02000a;
        header->siaddr = 0x0202000a;
        const uint8_t *request = (const uint8_t *)b;
        size_t at = 240;
        response[at++] = 53;
        response[at++] = 1;
        response[at++] = request[242] == DHCP_MSG_DISCOVER ? DHCP_MSG_OFFER : DHCP_MSG_ACK;
        if (wire_discovery) {
            response[at++] = DHCP_OPT_TZ_IANA;
            response[at++] = 15;
            memcpy(response + at, "Europe/Brussels", 15);
            at += 15;
        }
        response[at++] = DHCP_OPT_END;
        response_size = at;
        return c;
    }
    if (nr == SYS_RECVFROM) {
        assert(a == 8 && c >= response_size && response_size);
        memcpy((void *)b, response, response_size);
        *(net_sockaddr_in_t *)e = (net_sockaddr_in_t){
            .family = NET_AF_INET, .port = __builtin_bswap16(67), .address = 0x0202000a};
        *(uint32_t *)f = sizeof(net_sockaddr_in_t);
        size_t size = response_size;
        response_size = 0;
        return size;
    }
    if (nr == SYS_OPEN) {
        const char *path = (const char *)a;
        if (!strcmp(path, "/usr/share/zoneinfo/Europe/Brussels")) {
            if (packed_mode)
                return SYSCALL_ENOENT;
            if (readonly_failure)
                return SYSCALL_EIO;
            position = 0;
            read_source = zone;
            read_length = zone_len;
            return 3;
        }
        if (!strcmp(path, "/usr/share/zoneinfo.tzdb") && packed_mode) {
            if (readonly_failure)
                return SYSCALL_EIO;
            position = 0;
            read_source = database;
            read_length = database_len;
            return 3;
        }
        if (!strcmp(path, "/run/timezone.new")) {
            pending[0] = 0;
            pending_len = 0;
            return 4;
        }
        return SYSCALL_ENOENT;
    }
    if (nr == SYS_READ) {
        assert(a == 3);
        size_t count = read_length - position;
        if (count > c)
            count = c;
        if (count > 17)
            count = 17;
        memcpy((void *)b, read_source + position, count);
        position += count;
        return count;
    }
    if (nr == SYS_WRITE) {
        if (a == 1 || a == 2)
            return c;
        assert(a == 4);
        if (write_failure)
            return SYSCALL_EIO;
        size_t count = c < 3 ? c : 3;
        assert(pending_len + count < sizeof(pending));
        memcpy(pending + pending_len, (const void *)b, count);
        pending_len += count;
        pending[pending_len] = 0;
        return count;
    }
    if (nr == SYS_CLOSE)
        return 0;
    if (nr == SYS_RENAME) {
        assert(!strcmp((const char *)a, "/run/timezone.new") &&
               !strcmp((const char *)b, "/run/timezone"));
        if (rename_failure)
            return SYSCALL_EIO;
        copy(published, pending);
        pending[0] = 0;
        return 0;
    }
    if (nr == SYS_UNLINK) {
        if (!strcmp((const char *)a, "/run/timezone"))
            published[0] = 0;
        else {
            assert(!strcmp((const char *)a, "/run/timezone.new"));
            pending[0] = 0;
        }
        return 0;
    }
    if (nr == SYS_NETCTL)
        return net_result;
    assert(!"unexpected syscall in DHCP timezone publication");
    return SYSCALL_ENOSYS;
}
long tool_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    return mocked(nr, a, b, c, 0, 0, 0);
}
int main(void) {
    FILE *file = fopen("build/zoneinfo/Europe/Brussels", "rb");
    assert(file);
    zone_len = fread(zone, 1, sizeof(zone), file);
    fclose(file);
    dhcp_offer_t offer = {0};
    copy(offer.timezone_iana, "Europe/Brussels");
    copy(offer.timezone_posix, "UTC5");
    save_timezone(&offer);
    assert(!strcmp(published, "Europe/Brussels") && !pending[0]);
    copy(offer.timezone_iana, "Europe/Unknown");
    save_timezone(&offer);
    assert(!strcmp(published, "UTC5"));
    copy(offer.timezone_iana, "../../etc/shadow");
    copy(offer.timezone_posix, "invalid");
    save_timezone(&offer);
    assert(!published[0]);
    file = fopen("build/zoneinfo.tzdb", "rb");
    assert(file);
    database_len = fread(database, 1, sizeof(database), file);
    fclose(file);
    assert(database_len < sizeof(database));
    packed_mode = true;
    copy(offer.timezone_iana, "Europe/Brussels");
    save_timezone(&offer);
    assert(!strcmp(published, "Europe/Brussels"));
    size_t length = database_len;
    database_len = 11;
    save_timezone(&offer);
    assert(!published[0]);
    database_len = length;
    database[8] = 0xff;
    save_timezone(&offer);
    assert(!published[0]);
    database[8] = 0;
    copy(offer.timezone_iana, "Asia/Kathmandu");
    save_timezone(&offer);
    assert(!strcmp(published, "Asia/Kathmandu"));
    copy(offer.timezone_iana, "Europe/Brussels");
    offer.timezone_posix[0] = 0;
    readonly_failure = true;
    save_timezone(&offer);
    assert(!published[0]);
    readonly_failure = false;
    write_failure = true;
    save_timezone(&offer);
    assert(!published[0] && !pending[0]);
    write_failure = false;
    rename_failure = true;
    save_timezone(&offer);
    assert(!published[0] && !pending[0]);
    rename_failure = false;
    assert(apply_interface(&offer) && !strcmp(published, "Europe/Brussels"));
    offer = (dhcp_offer_t){0};
    net_result = SYSCALL_EIO;
    assert(!apply_interface(&offer) && !strcmp(published, "Europe/Brussels"));
    net_result = 0;
    assert(apply_interface(&offer) && !published[0]);
    copy(published, "stale");
    flush_interface();
    assert(!published[0]);
    const uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
    dhcp_offer_t received;
    assert(dhcp_do_handshake(mac, 3, false, &received) == 0);
    assert(!strcmp(received.timezone_iana, "Europe/Brussels"));
    assert(apply_interface(&received) && !strcmp(published, "Europe/Brussels"));
    dhcp_offer_t renewed;
    assert(dhcp_do_renew(&received, mac, 3, &renewed) == 0 &&
           !strcmp(renewed.timezone_iana, "Europe/Brussels"));
    assert(dhcp_do_rebind(&received, mac, 3, &renewed) == 0 &&
           !strcmp(renewed.timezone_iana, "Europe/Brussels"));
    assert(dhcp_do_reboot(&received, mac, 3, &renewed) == 0 &&
           !strcmp(renewed.timezone_iana, "Europe/Brussels"));
    wire_discovery = false;
    assert(dhcp_do_handshake(mac, 3, false, &received) == 0 && !received.timezone_iana[0]);
    assert(apply_interface(&received) && !published[0]);
    puts("PASS actual DHCP discovery publication: IANA validation, POSIX fallback, absent/invalid "
         "removal, I/O failures and lease flush");
}
