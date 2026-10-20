#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "common.h"

extern int date_main(int argc, char **argv, const char *const *envp);
extern int sleep_main(int argc, char **argv);

static timespec_t mock_realtime;
static long mock_clock_ret = 0;

static timespec_t mock_sleep_req;
static timespec_t mock_sleep_rem;
static long mock_sleep_ret = 0;

static char mock_stdout[4096];
static size_t mock_stdout_len = 0;
static char mock_stderr[4096];
static size_t mock_stderr_len = 0;
static const char *mock_zonefile,*mock_dhcpzone;
static bool mock_dhcp_optin;
static const char *read_data;
static size_t read_position;

long tool_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    (void)c;
    if(nr==SYS_OPEN) {
        const char *path=(const char *)a;
        read_data=NULL;read_position=0;
        if(!strcmp(path,"/etc/timezone"))read_data=mock_zonefile;
        if(!strcmp(path,"/run/timezone"))read_data=mock_dhcpzone;
        if(!strcmp(path,"/etc/timezone.dhcp")&&mock_dhcp_optin)read_data="";
        return read_data?3:SYSCALL_ENOENT;
    }
    if(nr==SYS_READ) {
        assert(a==3&&read_data);size_t left=strlen(read_data)-read_position;
        size_t count=left<c?left:c;if(count>3)count=3; /* force short reads */
        memcpy((void *)b,read_data+read_position,count);read_position+=count;return count;
    }
    if(nr==SYS_CLOSE)return 0;
    if (nr == SYS_CLOCK_GETTIME) {
        if (mock_clock_ret < 0) return mock_clock_ret;
        if (a == CLOCK_REALTIME) {
            timespec_t *out = (timespec_t *)b;
            if (out) *out = mock_realtime;
            return 0;
        }
        return SYSCALL_EINVAL;
    }
    if (nr == SYS_NANOSLEEP) {
        if (a) mock_sleep_req = *(const timespec_t *)a;
        if (b) *(timespec_t *)b = mock_sleep_rem;
        return mock_sleep_ret;
    }
    if (nr == SYS_WRITE) {
        int fd = (int)a;
        const char *data = (const char *)b;
        size_t len = (size_t)c;
        if (fd == 1) {
            if (mock_stdout_len + len < sizeof(mock_stdout)) {
                memcpy(mock_stdout + mock_stdout_len, data, len);
                mock_stdout_len += len;
                mock_stdout[mock_stdout_len] = '\0';
            }
            return (long)len;
        }
        if (fd == 2) {
            if (mock_stderr_len + len < sizeof(mock_stderr)) {
                memcpy(mock_stderr + mock_stderr_len, data, len);
                mock_stderr_len += len;
                mock_stderr[mock_stderr_len] = '\0';
            }
            return (long)len;
        }
    }
    return SYSCALL_ENOSYS;
}

static void reset_buffers(void) {
    mock_stdout_len = 0;
    mock_stdout[0] = '\0';
    mock_stderr_len = 0;
    mock_stderr[0] = '\0';
    mock_clock_ret = 0;
    mock_sleep_ret = 0;
    memset(&mock_sleep_req, 0, sizeof(mock_sleep_req));
    memset(&mock_sleep_rem, 0, sizeof(mock_sleep_rem));
}

static void test_date_tool(void) {
    printf("Testing /bin/date tool...\n");

    /* 2026-10-16 08:30:45 UTC */
    mock_realtime.tv_sec = 1792139445;
    mock_realtime.tv_nsec = 123456789;

    /* 1. Default date output */
    reset_buffers();
    char *argv_default[] = {"date", NULL};
    int ret = date_main(1, argv_default, NULL);
    assert(ret == 0);
    assert(strcmp(mock_stdout, "Fri Oct 16 08:30:45 UTC 2026\n") == 0);

    /* 2. ISO-8601 mode */
    reset_buffers();
    char *argv_iso[] = {"date", "-I", NULL};
    ret = date_main(2, argv_iso, NULL);
    assert(ret == 0);
    assert(strcmp(mock_stdout, "2026-10-16T08:30:45Z\n") == 0);

    /* 3. RFC-2822 mode */
    reset_buffers();
    char *argv_rfc[] = {"date", "-R", NULL};
    ret = date_main(2, argv_rfc, NULL);
    assert(ret == 0);
    assert(strcmp(mock_stdout, "Fri, 16 Oct 2026 08:30:45 +0000\n") == 0);

    /* 4. Seconds since epoch (+%s) */
    reset_buffers();
    char *argv_epoch[] = {"date", "+%s", NULL};
    ret = date_main(2, argv_epoch, NULL);
    assert(ret == 0);
    assert(strcmp(mock_stdout, "1792139445\n") == 0);

    /* 5. Help */
    reset_buffers();
    char *argv_help[] = {"date", "--help", NULL};
    ret = date_main(2, argv_help, NULL);
    assert(ret == 0);
    assert(strstr(mock_stdout, "Usage: date") != NULL);

    /* 6. Invalid option */
    reset_buffers();
    char *argv_bad[] = {"date", "--bogus", NULL};
    ret = date_main(2, argv_bad, NULL);
    assert(ret != 0);

    const char *east[]={"TZ=+05:45",NULL};
    reset_buffers();assert(date_main(2,argv_iso,east)==0);
    assert(!strcmp(mock_stdout,"2026-10-16T14:15:45+05:45\n"));
    const char *west[]={"TZ=UTC5",NULL};
    reset_buffers();assert(date_main(2,argv_rfc,west)==0);
    assert(!strcmp(mock_stdout,"Fri, 16 Oct 2026 03:30:45 -0500\n"));
    mock_zonefile="+02:00\n";mock_dhcpzone="-03:00";mock_dhcp_optin=true;
    reset_buffers();assert(date_main(2,argv_iso,NULL)==0);
    assert(!strcmp(mock_stdout,"2026-10-16T10:30:45+02:00\n"));
    reset_buffers();assert(date_main(2,argv_iso,west)==0);
    assert(!strcmp(mock_stdout,"2026-10-16T03:30:45-05:00\n"));
    mock_zonefile=NULL;
    reset_buffers();assert(date_main(2,argv_iso,NULL)==0);
    assert(!strcmp(mock_stdout,"2026-10-16T05:30:45-03:00\n"));
    mock_dhcp_optin=false;
    reset_buffers();assert(date_main(2,argv_iso,NULL)==0);
    assert(!strcmp(mock_stdout,"2026-10-16T08:30:45Z\n"));
    mock_zonefile="../../etc/shadow";
    reset_buffers();assert(date_main(2,argv_iso,NULL)!=0&&mock_stdout_len==0);
    char *forceutc[]={"date","-I","-u",NULL};
    reset_buffers();assert(date_main(3,forceutc,NULL)==0);
    assert(!strcmp(mock_stdout,"2026-10-16T08:30:45Z\n"));
    reset_buffers();assert(date_main(2,argv_epoch,NULL)==0);
    assert(!strcmp(mock_stdout,"1792139445\n"));
    mock_zonefile=NULL;mock_dhcpzone=NULL;
    mock_realtime.tv_sec=0;
    reset_buffers();assert(date_main(2,argv_iso,west)==0);
    assert(!strcmp(mock_stdout,"1969-12-31T19:00:00-05:00\n"));
    mock_realtime.tv_sec=-1;
    reset_buffers();assert(date_main(2,argv_epoch,NULL)==0);
    assert(!strcmp(mock_stdout,"-1\n"));

    printf("  [PASS] date tool verified.\n");
}

static void test_sleep_tool(void) {
    printf("Testing /bin/sleep tool...\n");

    /* 1. Basic integer seconds */
    reset_buffers();
    char *argv1[] = {"sleep", "2", NULL};
    int ret = sleep_main(2, argv1);
    assert(ret == 0);
    assert(mock_sleep_req.tv_sec == 2);
    assert(mock_sleep_req.tv_nsec == 0);

    /* 2. Fractional seconds */
    reset_buffers();
    char *argv2[] = {"sleep", "0.5", NULL};
    ret = sleep_main(2, argv2);
    assert(ret == 0);
    assert(mock_sleep_req.tv_sec == 0);
    assert(mock_sleep_req.tv_nsec == 500000000L);

    /* 3. Suffix minutes (1.5m = 90s) */
    reset_buffers();
    char *argv3[] = {"sleep", "1.5m", NULL};
    ret = sleep_main(2, argv3);
    assert(ret == 0);
    assert(mock_sleep_req.tv_sec == 90);
    assert(mock_sleep_req.tv_nsec == 0);

    /* 4. Multiple intervals */
    reset_buffers();
    char *argv4[] = {"sleep", "1", "0.25", NULL};
    ret = sleep_main(3, argv4);
    assert(ret == 0);
    assert(mock_sleep_req.tv_sec == 1);
    assert(mock_sleep_req.tv_nsec == 250000000L);

    /* 5. Help */
    reset_buffers();
    char *argv_help[] = {"sleep", "--help", NULL};
    ret = sleep_main(2, argv_help);
    assert(ret == 0);
    assert(strstr(mock_stdout, "usage: sleep") != NULL);

    /* 6. Missing operand */
    reset_buffers();
    char *argv_none[] = {"sleep", NULL};
    ret = sleep_main(1, argv_none);
    assert(ret != 0);

    /* 7. Invalid interval */
    reset_buffers();
    char *argv_invalid[] = {"sleep", "xyz", NULL};
    ret = sleep_main(2, argv_invalid);
    assert(ret != 0);

    /* 8. Interrupted by signal */
    reset_buffers();
    mock_sleep_ret = SYSCALL_EINTR;
    char *argv_intr[] = {"sleep", "5", NULL};
    ret = sleep_main(2, argv_intr);
    assert(ret == 130);

    printf("  [PASS] sleep tool verified.\n");
}

int main(void) {
    printf("=== Date & Sleep Userland Tool Host Tests ===\n");
    test_date_tool();
    test_sleep_tool();
    printf(">>> ALL DATE & SLEEP HOST TESTS PASSED (100%%) <<<\n");
    return 0;
}
