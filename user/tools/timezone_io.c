#include "../timezone.h"
#include "common.h"
static uint8_t tz_bytes[16384];
static char setting[256];
static bool exact(long fd, void *data, size_t size) {
    size_t at = 0;
    while (at < size) {
        long n = tool_syscall(SYS_READ, fd, (uintptr_t)((uint8_t *)data + at), size - at);
        if (n <= 0 || (size_t)n > size - at)
            return false;
        at += (size_t)n;
    }
    return true;
}
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static bool packed(const char *name, int64_t s, tz_result_t *out) {
    long fd = tool_syscall(SYS_OPEN, (uintptr_t) "/usr/share/zoneinfo.tzdb",
                           VFS_O_RDONLY | VFS_O_CLOEXEC, 0);
    if (fd < 0)
        return false;
    uint8_t head[12];
    bool ok = false;
    if (!exact(fd, head, sizeof(head)))
        goto done;
    const char *magic = "FOSTZ001";
    for (int i = 0; i < 8; i++)
        if (head[i] != (uint8_t)magic[i])
            goto done;
    uint32_t count = be32(head + 8);
    size_t total = 12;
    if (!count || count > 1024)
        goto done;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t entry[5];
        char label[128];
        if (!exact(fd, entry, 5))
            goto done;
        size_t length = entry[0], bytes = be32(entry + 1);
        if (!length || length >= sizeof(label) || !bytes || bytes >= sizeof(tz_bytes))
            goto done;
        total += 5 + length + bytes;
        if (total > 1024 * 1024)
            goto done;
        if (!exact(fd, label, length))
            goto done;
        for (size_t j = 0; j < length; j++)
            if (!label[j])
                goto done;
        label[length] = 0;
        if (!tz_name(label))
            goto done;
        if (!exact(fd, tz_bytes, bytes))
            goto done;
        size_t match = 0;
        while (label[match] && label[match] == name[match])
            ++match;
        if (label[match] == name[match]) {
            ok = tz_tzif(tz_bytes, bytes, s, out);
            goto done;
        }
    }
done:
    tool_syscall(SYS_CLOSE, fd, 0, 0);
    return ok;
}
/* Return absent=0, complete=1, error=-1. Reject truncation and embedded NUL. */
static int read_setting(const char *path) {
    long fd = tool_syscall(SYS_OPEN, (uintptr_t)path, VFS_O_RDONLY | VFS_O_CLOEXEC, 0);
    if (fd < 0)
        return fd == SYSCALL_ENOENT || fd == SYSCALL_ENOSYS ? 0 : -1;
    size_t used = 0;
    long n;
    while (used < sizeof(setting)) {
        n = tool_syscall(SYS_READ, fd, (uintptr_t)(setting + used), sizeof(setting) - used);
        if (n <= 0)
            break;
        used += (size_t)n;
    }
    tool_syscall(SYS_CLOSE, fd, 0, 0);
    if (n < 0 || used >= sizeof(setting))
        return -1;
    for (size_t i = 0; i < used; i++)
        if (!setting[i])
            return -1;
    while (used && (setting[used - 1] == '\n' || setting[used - 1] == '\r'))
        --used;
    setting[used] = 0;
    return 1;
}
bool tz_resolve(const char *const *envp, int64_t s, tz_result_t *out) {
    const char *spec = NULL;
    bool discovered = false;
    if (envp)
        for (size_t i = 0; i < 256 && envp[i]; i++) {
            const char *e = envp[i];
            if (e[0] == 'T' && e[1] == 'Z' && e[2] == '=') {
                spec = e + 3;
                break;
            }
        }
    if (!spec) {
        int r = read_setting("/etc/timezone");
        if (r < 0)
            return false;
        if (r)
            spec = setting;
        else {
            long fd = tool_syscall(SYS_OPEN, (uintptr_t) "/etc/timezone.dhcp",
                                   VFS_O_RDONLY | VFS_O_CLOEXEC, 0);
            if (fd >= 0) {
                tool_syscall(SYS_CLOSE, fd, 0, 0);
                r = read_setting("/run/timezone");
                if (r < 0)
                    return false;
                if (r) {
                    spec = setting;
                    discovered = true;
                }
            }
        }
    }
    if (!spec)
        spec = "UTC";
    if (tz_spec(spec, s, out))
        return true;
    return discovered && tz_posix("UTC", s, out);
}
bool tz_spec(const char *spec, int64_t s, tz_result_t *out) {
    if (!spec || !out)
        return false;
    size_t k = 0;
    while (k < 256 && spec[k])
        ++k;
    if (k == 256)
        return false;
    if (tz_posix(spec, s, out))
        return true;
    if (*spec == ':')
        ++spec;
    if (!tz_name(spec))
        return false;
    const char *name = spec;
    char path[160];
    size_t at = 0;
    const char *base = "/usr/share/zoneinfo/";
    while (*base)
        path[at++] = *base++;
    while (*spec)
        path[at++] = *spec++;
    path[at] = 0;
    long fd = tool_syscall(SYS_OPEN, (uintptr_t)path, VFS_O_RDONLY | VFS_O_CLOEXEC, 0);
    if (fd < 0)
        return fd == SYSCALL_ENOENT ? packed(name, s, out) : false;
    size_t used = 0;
    long n = 0;
    while (used < sizeof(tz_bytes)) {
        n = tool_syscall(SYS_READ, fd, (uintptr_t)(tz_bytes + used), sizeof(tz_bytes) - used);
        if (n <= 0)
            break;
        used += (size_t)n;
    }
    tool_syscall(SYS_CLOSE, fd, 0, 0);
    return n == 0 && used < sizeof(tz_bytes) && tz_tzif(tz_bytes, used, s, out);
}
