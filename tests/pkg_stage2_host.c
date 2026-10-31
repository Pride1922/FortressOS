/* Stage 2 host test: name-based download/cache, privileged verification,
 * and writable root install backend logic. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "user/pkg/system_cli.h"
#include "user/pkg/repository.h"
#include "user/tools/common.h"
#include "pkg_abi.h"
#include "crypto.h"

static uint8_t g_trust_conf[1024];
static size_t g_trust_len;
static uint8_t g_repo_conf[] = "version=1\nurl=https://pkg.fortress.org\n";
static size_t g_repo_len = sizeof(g_repo_conf) - 1;

static uint8_t g_catalog_v1[4096];
static size_t g_catalog_len;
static uint8_t g_rot13_fpk[4096];
static size_t g_rot13_len;

/* Virtual files in host mock filesystem */
typedef struct {
    char path[128];
    uint8_t data[4096];
    size_t size;
    size_t offset;
    bool open;
    uint32_t mode;
    uint32_t uid;
} mock_file_t;

#define MAX_MOCK_FILES 16
static mock_file_t mock_files[MAX_MOCK_FILES];
static unsigned mock_file_count;

static void safe_strcpy(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    size_t i = 0;
    while (i + 1 < cap && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static mock_file_t *find_file(const char *path) {
    for (unsigned i = 0; i < mock_file_count; i++) {
        if (!strcmp(mock_files[i].path, path)) return &mock_files[i];
    }
    return NULL;
}

static mock_file_t *create_file(const char *path, uint32_t mode, uint32_t uid) {
    mock_file_t *f = find_file(path);
    if (!f) {
        assert(mock_file_count < MAX_MOCK_FILES);
        f = &mock_files[mock_file_count++];
        memset(f, 0, sizeof *f);
        safe_strcpy(f->path, sizeof(f->path), path);
    }
    f->mode = mode;
    f->uid = uid;
    f->offset = 0;
    return f;
}

static unsigned open_fds;
static unsigned spawn_count;
static char last_spawn_cmd[128];
static char last_spawn_arg_url[256];
static char last_spawn_arg_dst[128];
static bool fail_spawn;
static bool fail_download_verify;
static bool simulate_identical_install;
static bool simulate_upgrade_install;
static bool simulate_base_collision;
static bool simulate_tampered_artifact;
static long injected_install_error;

static uint64_t g_session_counter = 100;
static uint64_t g_active_session = 0;
static unsigned g_install_source_calls = 0;
static char captured_output[32768];
static size_t captured_bytes;

long tool_syscall(long call, uintptr_t a, uintptr_t b, uintptr_t c) {
    if (call == SYS_WRITE) {
        if (a == 1 || a == 2) {
            if(c<sizeof captured_output-captured_bytes) {
                memcpy(captured_output+captured_bytes,(const void *)b,c);
                captured_bytes+=c;captured_output[captured_bytes]=0;
            }
            return (long)fwrite((const void *)b, 1, (size_t)c, a == 1 ? stdout : stderr);
        }
        return -VFS_EBADF;
    }
    if (call == SYS_CLOCK_GETTIME) {
        assert(a == CLOCK_REALTIME);
        *(timespec_t *)b = (timespec_t){1793188800, 0};
        return 0;
    }
    if (call == SYS_MKDIR) {
        return 0; /* All dirs succeed */
    }
    if (call == SYS_UNLINK) {
        const char *p = (const char *)a;
        for (unsigned i = 0; i < mock_file_count; i++) {
            if (!strcmp(mock_files[i].path, p)) {
                mock_files[i] = mock_files[--mock_file_count];
                return 0;
            }
        }
        return 0;
    }
    if (call == SYS_RENAME_REPLACE || call == SYS_RENAME) {
        const char *src = (const char *)a;
        const char *dst = (const char *)b;
        mock_file_t *sf = find_file(src);
        if (!sf) return -VFS_ENOENT;
        mock_file_t *df = find_file(dst);
        if (!df) df = create_file(dst, sf->mode, sf->uid);
        memcpy(df->data, sf->data, sf->size);
        df->size = sf->size;
        df->mode = sf->mode;
        df->uid = sf->uid;
        for (unsigned i = 0; i < mock_file_count; i++) {
            if (&mock_files[i] == sf) {
                mock_files[i] = mock_files[--mock_file_count];
                break;
            }
        }
        return 0;
    }
    if (call == SYS_OPEN) {
        const char *p = (const char *)a;
        uint32_t flags = (uint32_t)b;
        if (!strcmp(p, "/var/cache/pkg/.probe")) {
            return 3; /* Probe succeeds */
        }
        mock_file_t *f = find_file(p);
        if (!f) {
            if (flags & VFS_O_CREAT) {
                f = create_file(p, (uint32_t)c, 0);
            } else {
                return -VFS_ENOENT;
            }
        }
        if (f->open) return -1;
        f->open = true;
        f->offset = 0;
        open_fds++;
        return (long)(f - mock_files + 4);
    }
    if (call == SYS_CLOSE) {
        int fd = (int)a;
        if (fd == 3) return 0;
        assert(fd >= 4 && fd < (int)(mock_file_count + 4));
        mock_file_t *f = &mock_files[fd - 4];
        assert(f->open);
        f->open = false;
        open_fds--;
        return 0;
    }
    if (call == SYS_FSTAT) {
        int fd = (int)a;
        stat_ext_v1_t *st = (stat_ext_v1_t *)b;
        assert(fd >= 4 && fd < (int)(mock_file_count + 4));
        mock_file_t *f = &mock_files[fd - 4];
        *st = (stat_ext_v1_t){
            .size = sizeof *st,
            .version = 1,
            .type = VFS_FILE,
            .file_size = f->size,
            .mode = f->mode ? f->mode : 0644,
            .uid = f->uid,
            .gid = 0
        };
        return 0;
    }
    if (call == SYS_READ) {
        int fd = (int)a;
        assert(fd >= 4 && fd < (int)(mock_file_count + 4));
        mock_file_t *f = &mock_files[fd - 4];
        size_t cap = c;
        if (f->offset >= f->size) return 0;
        size_t n = f->size - f->offset;
        if (n > cap) n = cap;
        memcpy((void *)b, f->data + f->offset, n);
        f->offset += n;
        return (long)n;
    }
    if (call == SYS_SPAWN) {
        const char *prog = (const char *)a;
        const char *const *argv = (const char *const *)b;
        spawn_count++;
        safe_strcpy(last_spawn_cmd, sizeof(last_spawn_cmd), prog);
        if (argv && argv[0] && argv[1] && argv[2] && argv[3] && argv[4]) {
            safe_strcpy(last_spawn_arg_dst, sizeof(last_spawn_arg_dst), argv[3]);
            safe_strcpy(last_spawn_arg_url, sizeof(last_spawn_arg_url), argv[4]);
            /* Simulate wget writing the destination file */
            if (!fail_spawn) {
                mock_file_t *f = create_file(argv[3], 0644, 0);
                if (strstr(argv[4], "catalog.v1")) {
                    memcpy(f->data, g_catalog_v1, g_catalog_len);
                    f->size = g_catalog_len;
                    if (fail_download_verify) f->data[g_catalog_len - 1] ^= 1;
                } else if (strstr(argv[4], ".fpk")) {
                    memcpy(f->data, g_rot13_fpk, g_rot13_len);
                    f->size = g_rot13_len;
                    if (fail_download_verify) f->data[g_rot13_len - 1] ^= 1;
                }
            }
        }
        return fail_spawn ? -VFS_EIO : 42;
    }
    if (call == SYS_WAIT) {
        int64_t *status = (int64_t *)b;
        *status = fail_spawn ? 1 : 0;
        return 0;
    }
    if (call == SYS_PKGCTL) {
        const pkg_request_v1 *q = (const pkg_request_v1 *)a;
        pkg_result_v1 *r = (pkg_result_v1 *)b;
        if (q->operation == PKG_ACQUIRE) {
            assert(q->version == 1);
            g_active_session = ++g_session_counter;
            *r = (pkg_result_v1){
                .size = sizeof *r,
                .version = 1,
                .session = g_active_session,
                .sequence = 10,
                .account_sequence = 1,
                .credential_generation = 1,
                .phase = PKG_PHASE_IDLE
            };
            return 0;
        }
        if (q->operation == PKG_RELEASE) {
            assert(q->version == 1 && q->session == g_active_session);
            g_active_session = 0;
            *r = (pkg_result_v1){
                .size = sizeof *r,
                .version = 1,
                .session = 0,
                .sequence = 11,
                .account_sequence = 1,
                .credential_generation = 1,
                .phase = PKG_PHASE_IDLE
            };
            return 0;
        }
        if (q->operation == PKG_INSTALL_SOURCE) {
            const pkg_request_v2 *s = (const pkg_request_v2 *)a;
            (void)s;
            assert(q->version == 2 && q->session == g_active_session);
            g_install_source_calls++;

            if(injected_install_error)return injected_install_error;
            if (simulate_tampered_artifact) return SYSCALL_EACCES;
            if (simulate_base_collision) return SYSCALL_EACCES;
            if (simulate_upgrade_install) return SYSCALL_EACCES;

            *r = (pkg_result_v1){
                .size = sizeof *r,
                .version = 1,
                .session = g_active_session,
                .sequence = simulate_identical_install ? 10 : 11,
                .account_sequence = 1,
                .credential_generation = 1,
                .phase = PKG_PHASE_IDLE
            };
            return 0;
        }
        return SYSCALL_EINVAL;
    }
    return -VFS_EINVAL;
}

static void init_mock_fs(void) {
    captured_bytes=0;captured_output[0]=0;
    mock_file_count = 0;
    open_fds = 0;
    spawn_count = 0;
    g_install_source_calls = 0;
    fail_spawn = false;
    fail_download_verify = false;
    simulate_identical_install = false;
    simulate_upgrade_install = false;
    simulate_base_collision = false;
    simulate_tampered_artifact = false;
    injected_install_error=0;

    /* Populate /etc/pkg/repository.conf */
    mock_file_t *rf = create_file("/etc/pkg/repository.conf", 0644, 0);
    memcpy(rf->data, g_repo_conf, g_repo_len);
    rf->size = g_repo_len;

    /* Populate /etc/pkg/trust.conf */
    mock_file_t *tf = create_file("/etc/pkg/trust.conf", 0644, 0);
    memcpy(tf->data, g_trust_conf, g_trust_len);
    tf->size = g_trust_len;
}

static size_t read_file_disk(const char *path, uint8_t *out, size_t cap) {
    FILE *f = fopen(path, "rb");
    assert(f);
    size_t n = fread(out, 1, cap, f);
    assert(n > 0 && n < cap && !ferror(f));
    fclose(f);
    return n;
}

int main(int argc, char **argv) {
    assert(argc == 4);
    g_trust_len = read_file_disk(argv[1], g_trust_conf, sizeof g_trust_conf);
    g_catalog_len = read_file_disk(argv[2], g_catalog_v1, sizeof g_catalog_v1);
    g_rot13_len = read_file_disk(argv[3], g_rot13_fpk, sizeof g_rot13_fpk);

    printf("Loaded trust: %zu bytes, catalog: %zu bytes, rot13 fpk: %zu bytes\n",
           g_trust_len, g_catalog_len, g_rot13_len);

    /* 1. Test 'pkg update' */
    {
        init_mock_fs();
        char *cmd[] = {"pkg", "update", NULL};
        int rc = pkg_system_main(2, cmd);
        assert(rc == 0);
        assert(spawn_count == 1);
        assert(!strcmp(last_spawn_cmd, "/bin/wget"));
        assert(!strcmp(last_spawn_arg_dst, "/var/cache/pkg/catalog.v1.tmp"));
        assert(find_file("/var/cache/pkg/catalog.v1") != NULL);
        assert(strstr(last_spawn_arg_url, "catalog.v1") != NULL);
        assert(open_fds == 0);
        puts("PASS: pkg update (downloads and verifies catalog into cache)");
    }

    /* 2. Test 'pkg update' with corrupted signature */
    {
        init_mock_fs();
        fail_download_verify = true;
        char *cmd[] = {"pkg", "update", NULL};
        int rc = pkg_system_main(2, cmd);
        assert(rc != 0); /* Verification failed */
        assert(open_fds == 0);
        puts("PASS: pkg update rejects invalid catalog signature");
    }

    /* 3. Test 'pkg get rot13' */
    {
        init_mock_fs();
        /* First ensure catalog is present in cache */
        mock_file_t *cat = create_file("/var/cache/pkg/catalog.v1", 0644, 0);
        memcpy(cat->data, g_catalog_v1, g_catalog_len);
        cat->size = g_catalog_len;

        char *cmd[] = {"pkg", "get", "rot13", NULL};
        int rc = pkg_system_main(3, cmd);
        assert(rc == 0);
        assert(spawn_count == 1);
        assert(!strcmp(last_spawn_cmd, "/bin/wget"));
        assert(!strcmp(last_spawn_arg_dst, "/var/cache/pkg/rot13.fpk.tmp"));
        assert(find_file("/var/cache/pkg/rot13.fpk") != NULL);
        assert(open_fds == 0);
        puts("PASS: pkg get rot13 (resolves content-addressed URL and verifies artifact into cache)");
    }

    /* 4. Test 'pkg get non-existent' */
    {
        init_mock_fs();
        mock_file_t *cat = create_file("/var/cache/pkg/catalog.v1", 0644, 0);
        memcpy(cat->data, g_catalog_v1, g_catalog_len);
        cat->size = g_catalog_len;

        char *cmd[] = {"pkg", "get", "no-such-package", NULL};
        int rc = pkg_system_main(3, cmd);
        assert(rc != 0);
        assert(spawn_count == 0);
        assert(open_fds == 0);
        puts("PASS: pkg get refuses unknown package");
    }

    /* 5. Test 'pkg get rot13' with corrupt artifact download */
    {
        init_mock_fs();
        mock_file_t *cat = create_file("/var/cache/pkg/catalog.v1", 0644, 0);
        memcpy(cat->data, g_catalog_v1, g_catalog_len);
        cat->size = g_catalog_len;

        fail_download_verify = true;
        char *cmd[] = {"pkg", "get", "rot13", NULL};
        int rc = pkg_system_main(3, cmd);
        assert(rc != 0);
        assert(open_fds == 0);
        puts("PASS: pkg get refuses corrupted artifact payload");
    }

    /* 6. Test 'pkg install rot13' (3 args: name-based from cache) */
    {
        init_mock_fs();
        /* Seed cached catalog and artifact */
        mock_file_t *cat = create_file("/var/cache/pkg/catalog.v1", 0644, 0);
        memcpy(cat->data, g_catalog_v1, g_catalog_len);
        cat->size = g_catalog_len;

        mock_file_t *art = create_file("/var/cache/pkg/rot13.fpk", 0644, 0);
        memcpy(art->data, g_rot13_fpk, g_rot13_len);
        art->size = g_rot13_len;

        char *cmd[] = {"pkg", "install", "rot13", NULL};
        int rc = pkg_system_main(3, cmd);
        assert(rc == 0);
        assert(g_install_source_calls == 1);
        assert(g_active_session == 0); /* Session acquired and released */
        assert(open_fds == 0);
        puts("PASS: pkg install rot13 (cached installation through PKG_INSTALL_SOURCE)");
    }

    /* 7. Test 'pkg install rot13 /staging' (4 args: explicit directory retained) */
    {
        init_mock_fs();
        mock_file_t *cat = create_file("/staging/catalog.v1", 0644, 0);
        memcpy(cat->data, g_catalog_v1, g_catalog_len);
        cat->size = g_catalog_len;

        mock_file_t *art = create_file("/staging/artifact.fpk", 0644, 0);
        memcpy(art->data, g_rot13_fpk, g_rot13_len);
        art->size = g_rot13_len;

        char *cmd[] = {"pkg", "install", "rot13", "/staging", NULL};
        int rc = pkg_system_main(4, cmd);
        assert(rc == 0);
        assert(g_install_source_calls == 1);
        assert(g_active_session == 0);
        assert(open_fds == 0);
        puts("PASS: pkg install rot13 /staging (explicit-directory fallback preserved)");
    }

    /* 8. Test privileged re-verification: tampered cached artifact */
    {
        init_mock_fs();
        mock_file_t *cat = create_file("/var/cache/pkg/catalog.v1", 0644, 0);
        memcpy(cat->data, g_catalog_v1, g_catalog_len);
        cat->size = g_catalog_len;

        mock_file_t *art = create_file("/var/cache/pkg/rot13.fpk", 0644, 0);
        memcpy(art->data, g_rot13_fpk, g_rot13_len);
        art->size = g_rot13_len;

        simulate_tampered_artifact = true;
        char *cmd[] = {"pkg", "install", "rot13", NULL};
        int rc = pkg_system_main(3, cmd);
        assert(rc != 0);
        assert(open_fds == 0);
        puts("PASS: privileged path refuses tampered cached artifact");
    }

    /* 9. Test refusal of base command collision */
    {
        init_mock_fs();
        mock_file_t *cat = create_file("/var/cache/pkg/catalog.v1", 0644, 0);
        memcpy(cat->data, g_catalog_v1, g_catalog_len);
        cat->size = g_catalog_len;

        mock_file_t *art = create_file("/var/cache/pkg/login.fpk", 0644, 0);
        memcpy(art->data, g_rot13_fpk, g_rot13_len);
        art->size = g_rot13_len;

        simulate_base_collision = true;
        char *cmd[] = {"pkg", "install", "login", NULL};
        int rc = pkg_system_main(3, cmd);
        assert(rc != 0);
        assert(open_fds == 0);
        puts("PASS: refuse base command collisions (e.g. login)");
    }

    /* 10. Test identical installed build is a no-op */
    {
        init_mock_fs();
        mock_file_t *cat = create_file("/var/cache/pkg/catalog.v1", 0644, 0);
        memcpy(cat->data, g_catalog_v1, g_catalog_len);
        cat->size = g_catalog_len;

        mock_file_t *art = create_file("/var/cache/pkg/rot13.fpk", 0644, 0);
        memcpy(art->data, g_rot13_fpk, g_rot13_len);
        art->size = g_rot13_len;

        simulate_identical_install = true;
        char *cmd[] = {"pkg", "install", "rot13", NULL};
        int rc = pkg_system_main(3, cmd);
        assert(rc == 0);
        assert(g_install_source_calls == 1);
        assert(open_fds == 0);
        assert(strstr(captured_output,"package is already installed rot13 v 1.0\n"));
        puts("PASS: identical installed build/hash is a no-op with package/version message");
    }

    /* 11. Test different version requires upgrade workflow (rejected in install) */
    {
        init_mock_fs();
        mock_file_t *cat = create_file("/var/cache/pkg/catalog.v1", 0644, 0);
        memcpy(cat->data, g_catalog_v1, g_catalog_len);
        cat->size = g_catalog_len;

        mock_file_t *art = create_file("/var/cache/pkg/rot13.fpk", 0644, 0);
        memcpy(art->data, g_rot13_fpk, g_rot13_len);
        art->size = g_rot13_len;

        simulate_upgrade_install = true;
        char *cmd[] = {"pkg", "install", "rot13", NULL};
        int rc = pkg_system_main(3, cmd);
        assert(rc != 0);
        assert(open_fds == 0);
        puts("PASS: different version installation rejected (upgrade required)");
    }

    /* Failed kernel calls must report the reason and release both sources/session. */
    const long errors[]={SYSCALL_EIO,SYSCALL_ENOSPC,SYSCALL_ENOMEM,SYSCALL_EAGAIN};
    const char *messages[]={"storage I/O failure","insufficient disk space","insufficient memory","backend is busy"};
    for(unsigned i=0;i<sizeof errors/sizeof *errors;i++) {
        init_mock_fs();
        mock_file_t *cat=create_file("/var/cache/pkg/catalog.v1",0644,0);
        memcpy(cat->data,g_catalog_v1,g_catalog_len);cat->size=g_catalog_len;
        mock_file_t *art=create_file("/var/cache/pkg/rot13.fpk",0644,0);
        memcpy(art->data,g_rot13_fpk,g_rot13_len);art->size=g_rot13_len;
        injected_install_error=errors[i];
        char *cmd[]={"pkg","install","rot13",NULL};
        assert(pkg_system_main(3,cmd)!=0&&!open_fds&&!g_active_session);
        assert(strstr(captured_output,messages[i]));
        assert(strstr(captured_output,"sudo pkg status"));
    }
    puts("PASS: I/O, no-space, memory and busy errors return with descriptors/session released");
    puts("\nALL STAGE 2 HOST TESTS PASSED (15/15)");
    return 0;
}
