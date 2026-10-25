#include "types.h"
#include "string.h"
#include "syscall_abi.h"
#include "socket_abi.h"
#include "terminal.h"
#include "vfs.h"
#include "hostkey_abi.h"
#include "ssh_hostkey.h"
#include "crypto.h"
#include "userdb.h"
#include "sshbuf.h"
#include "cipher.h"
#include "kex.h"
#include "transport.h"
#include "auth.h"
#include "monitor.h"
#include "channel.h"
#include "pty_bridge.h"
#include "sshd_config.h"
#include "sshd_readiness.h"
#include "sshd_admission.h"
#include "sshd_teardown.h"

#define SSHD_SERVER_VERSION "SSH-2.0-FortressSSH_0.1\r\n"
#define SSHD_SERVER_VERSION_TRIM "SSH-2.0-FortressSSH_0.1"

#ifndef SSHD_SYSCALL
static inline long sshd_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c,
                                uintptr_t d, uintptr_t e, uintptr_t f) {
#ifndef CRYPTO_HOST_TEST
    register uintptr_t r10 __asm__("r10") = d;
    register uintptr_t r8  __asm__("r8")  = e;
    register uintptr_t r9  __asm__("r9")  = f;
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c),
                     "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory", "cc");
    return nr;
#else
    (void)nr; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    return -1;
#endif
}
#define SSHD_SYSCALL sshd_syscall
#endif

static void sshd_write_err(const char *msg) {
    size_t len = 0; while (msg && msg[len]) len++;
    if (len > 0) {
        (void)SSHD_SYSCALL(SYS_WRITE, 1, (uintptr_t)"[sshd] ", 7, 0, 0, 0);
        (void)SSHD_SYSCALL(SYS_WRITE, 1, (uintptr_t)msg, len, 0, 0, 0);
        (void)SSHD_SYSCALL(SYS_WRITE, 1, (uintptr_t)"\n", 1, 0, 0, 0);
        (void)SSHD_SYSCALL(SYS_WRITE, 2, (uintptr_t)"[sshd] ", 7, 0, 0, 0);
        (void)SSHD_SYSCALL(SYS_WRITE, 2, (uintptr_t)msg, len, 0, 0, 0);
        (void)SSHD_SYSCALL(SYS_WRITE, 2, (uintptr_t)"\n", 1, 0, 0, 0);
    }
}


static volatile int g_terminate = 0;

/* Encryption has already consumed a sequence number. Complete these exact
 * bytes even if shutdown interrupted SEND; never append another packet after
 * a failed/partial frame. The monitor still bounds worker teardown at 5s. */
static int sshd_send_wire(ssh_transport_t *transport,int fd,const sshbuf_t *wire) {
    size_t off=0,len=sshbuf_len(wire);
    unsigned interruptions=0;
    while (off<len) {
        long n=SSHD_SYSCALL(SYS_SEND,(uintptr_t)fd,
                           (uintptr_t)(sshbuf_ptr(wire)+off),len-off,0,0,0);
        if (n==SYSCALL_EINTR && interruptions++<16) continue;
        if (n<=0 || (size_t)n>len-off) {
            transport->state=TRANSPORT_STATE_ERROR;
            transport->last_error=SSH_ERR_DISCONNECT;
            return SSH_ERR_DISCONNECT;
        }
        off+=(size_t)n;
    }
    return SSH_ERR_SUCCESS;
}

static void sshd_send_disconnect(ssh_transport_t *transport,int fd) {
    if (fd<0 || transport->state!=TRANSPORT_STATE_ACTIVE) return;
    static const uint8_t payload[]={0,0,0,11, 0,0,0,17,
        'S','y','s','t','e','m',' ','g','o','i','n','g',' ','d','o','w','n', 0,0,0,0};
    sshbuf_t wire;
    if (sshbuf_init(&wire,256,512)!=SSH_ERR_SUCCESS) return;
    if (ssh_transport_send_packet(transport,SSH_MSG_DISCONNECT,payload,sizeof(payload),&wire)==SSH_ERR_SUCCESS)
        (void)sshd_send_wire(transport,fd,&wire);
    sshbuf_free(&wire);
}

/* Drain buffered packets before reading again: NEWKEYS and the first encrypted
 * service request can arrive in the same TCP read (for example from PuTTY). */
static int sshd_receive_packet(ssh_transport_t *transport, int client_fd,
                               uint8_t *rx_buf, size_t rx_cap, uint8_t *ptype,
                               const uint8_t **payload, size_t *plen) {
    while (!g_terminate) {
        int rc = ssh_transport_get_packet(transport, ptype, payload, plen);
        if (rc != SSH_ERR_AGAIN) return rc;

        long n = SSHD_SYSCALL(SYS_RECV, (uintptr_t)client_fd,
                              (uintptr_t)rx_buf, rx_cap, 0, 0, 0);
        if (n <= 0) return SSH_ERR_DISCONNECT;
        rc = ssh_transport_feed_input(transport, rx_buf, (size_t)n);
        if (rc != SSH_ERR_SUCCESS) return rc;
    }
    return SSH_ERR_DISCONNECT;
}

static void on_sigterm(int sig) {
    (void)sig;
    g_terminate = 1;
}

static int sys_kill_wrapper(int64_t pid, int sig) {
    return (int)SSHD_SYSCALL(SYS_KILL, (uintptr_t)pid, (uintptr_t)sig, 0, 0, 0, 0);
}

static int64_t sys_waitpid_wrapper(int64_t pid, uint64_t *status, int options) {
    return (int64_t)SSHD_SYSCALL(SYS_WAITPID, (uintptr_t)pid, (uintptr_t)status, (uintptr_t)options, 0, 0, 0);
}

static int sys_close_wrapper(int fd) {
    return (int)SSHD_SYSCALL(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
}



static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int send_channel_replies(ssh_transport_t *transport, int client_fd, sshbuf_t *chan_reply) {
    while (sshbuf_len(chan_reply) > 0) {
        uint8_t rtype = 0;
        if (sshbuf_get_u8(chan_reply, &rtype) != 0) break;

        size_t plen = 0;
        size_t rem = sshbuf_len(chan_reply);
        const uint8_t *p = sshbuf_ptr(chan_reply);

        if (rtype == SSH_MSG_CHANNEL_EOF || rtype == SSH_MSG_CHANNEL_CLOSE ||
            rtype == SSH_MSG_CHANNEL_SUCCESS || rtype == SSH_MSG_CHANNEL_FAILURE) {
            plen = 4;
        } else if (rtype == SSH_MSG_CHANNEL_WINDOW_ADJUST) {
            plen = 8;
        } else if (rtype == SSH_MSG_CHANNEL_OPEN_CONFIRMATION) {
            plen = 16;
        } else if (rtype == SSH_MSG_REQUEST_FAILURE) {
            plen = 0;
        } else if (rtype == SSH_MSG_CHANNEL_DATA) {
            if (rem >= 8) {
                uint32_t dlen = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 8) | p[7];
                plen = 4 + 4 + dlen;
            } else {
                plen = rem;
            }
        } else if (rtype == SSH_MSG_CHANNEL_EXTENDED_DATA) {
            if (rem >= 12) {
                uint32_t dlen = ((uint32_t)p[8] << 24) | ((uint32_t)p[9] << 16) | ((uint32_t)p[10] << 8) | p[11];
                plen = 4 + 4 + 4 + dlen;
            } else {
                plen = rem;
            }
        } else if (rtype == SSH_MSG_CHANNEL_REQUEST) {
            if (rem >= 8) {
                uint32_t nlen = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 8) | p[7];
                plen = 4 + 4 + nlen + 1 + 4;
            } else {
                plen = rem;
            }
        } else {
            plen = rem;
        }

        if (plen > rem) return SSH_ERR_INVALID_FORMAT;

        sshbuf_t out_wire;
        /* PTY data alone can be 4096 bytes. Include channel framing, packet
         * padding, the encrypted length and the authentication tag. */
        int rc = sshbuf_init(&out_wire, 512, SSH_MAX_PACKET_LEN + 4 + SSH_POLY1305_TAG_LEN);
        if (rc != SSH_ERR_SUCCESS) return rc;
        rc = ssh_transport_send_packet(transport, rtype, p, plen, &out_wire);
        if (rc == SSH_ERR_SUCCESS) {
            rc = sshd_send_wire(transport,client_fd,&out_wire);
        }
        sshbuf_free(&out_wire);
        if (rc != SSH_ERR_SUCCESS) {
            transport->state=TRANSPORT_STATE_ERROR;
            transport->last_error=rc;
            return rc;
        }

        (void)sshbuf_consume(chan_reply, plen);
    }
    return SSH_ERR_SUCCESS;
}

/* Worker Process: runs the connection protocol under dropped capabilities */
static __attribute__((noinline)) int sshd_worker_run(int session_id, int client_fd, int mon_in_fd, int mon_out_fd, const char *pub_hex) {
    (void)session_id;
    /* These descriptors were explicitly transferred to the worker. They
     * remain private when it later starts an authenticated shell. */
    if (SSHD_SYSCALL(SYS_FCNTL, (uintptr_t)client_fd, F_SETFD, FD_CLOEXEC, 0, 0, 0) < 0 ||
        SSHD_SYSCALL(SYS_FCNTL, (uintptr_t)mon_in_fd, F_SETFD, FD_CLOEXEC, 0, 0, 0) < 0 ||
        SSHD_SYSCALL(SYS_FCNTL, (uintptr_t)mon_out_fd, F_SETFD, FD_CLOEXEC, 0, 0, 0) < 0)
        return 1;
    /* Setup signal handlers so SIGTERM breaks worker poll loop and runs clean pty_bridge_cleanup() */
    signal_action_t sa = { .handler = (uintptr_t)on_sigterm, .mask = 0, .flags = 0, .reserved = 0 };
    (void)SSHD_SYSCALL(SYS_SIGACTION, SIGTERM, (uintptr_t)&sa, 0, 0, 0, 0);
    (void)SSHD_SYSCALL(SYS_SIGACTION, SIGINT, (uintptr_t)&sa, 0, 0, 0, 0);

    /* Step 1: Drop capabilities down to CAP_SETUID | CAP_SETGID */
    (void)SSHD_SYSCALL(SYS_CAPSET, (1ULL << 4) | (1ULL << 5), 0, 0, 0, 0, 0);

    /* Step 2: Initialize protocol state */
    static ssh_transport_t transport;
    if (ssh_transport_init(&transport) != 0) return 1;

    static ssh_channel_mgr_t chan_mgr;
    channel_mgr_init(&chan_mgr);

    static pty_bridge_t bridge;
    pty_bridge_init(&bridge, 0, 80, 24);

    /* Step 3: Identification exchange */
    /* Send server version */
    long send_ver_rc = SSHD_SYSCALL(SYS_SEND, (uintptr_t)client_fd, (uintptr_t)SSHD_SERVER_VERSION, strlen(SSHD_SERVER_VERSION), 0, 0, 0);
    if (send_ver_rc < 0) return 1;

    /* Read client version */
    static char client_ver[256];
    size_t cv_len = 0;
    while (cv_len + 1 < sizeof(client_ver)) {
        static char s_ch;
        s_ch = 0;
        long n = SSHD_SYSCALL(SYS_RECV, (uintptr_t)client_fd, (uintptr_t)&s_ch, 1, 0, 0, 0);
        if (n <= 0) break;
        client_ver[cv_len++] = s_ch;
        if (s_ch == '\n') break;
    }
    client_ver[cv_len] = '\0';
    while (cv_len > 0 && (client_ver[cv_len - 1] == '\r' || client_ver[cv_len - 1] == '\n')) {
        client_ver[--cv_len] = '\0';
    }

    /* Step 4: KEXINIT exchange */
    static uint8_t cookie[16];
    if (SSHD_SYSCALL(SYS_GETRANDOM, (uintptr_t)cookie, sizeof(cookie), 0, 0, 0, 0) != sizeof(cookie)) return 1;

    sshbuf_t kexinit_wire;
    if (sshbuf_init(&kexinit_wire, 512, 4096) != 0) return 1;
    (void)ssh_transport_start_rekey(&transport, cookie, &kexinit_wire);
    (void)SSHD_SYSCALL(SYS_SEND, (uintptr_t)client_fd, (uintptr_t)sshbuf_ptr(&kexinit_wire), sshbuf_len(&kexinit_wire), 0, 0, 0);
    sshbuf_free(&kexinit_wire);

    /* Read client KEXINIT & KEX_ECDH_INIT */
    static uint8_t rx_buf[4096];
    bool kex_complete = false;
    uint8_t host_pub[32];
    memset(host_pub, 0, sizeof(host_pub));
    if (pub_hex && strlen(pub_hex) >= 64) {
        for (int k = 0; k < 32; k++) {
            int hi = hex_nibble(pub_hex[k * 2]);
            int lo = hex_nibble(pub_hex[k * 2 + 1]);
            if (hi >= 0 && lo >= 0) {
                host_pub[k] = (uint8_t)((hi << 4) | lo);
            }
        }
    } else {
        (void)sshd_read_host_public_key(host_pub);
    }
    uint8_t host_sig[64];
    bool host_sig_obtained = false;

    while (!kex_complete && !g_terminate) {
        long n = SSHD_SYSCALL(SYS_RECV, (uintptr_t)client_fd, (uintptr_t)rx_buf, sizeof(rx_buf), 0, 0, 0);
        if (n <= 0) break;
        (void)ssh_transport_feed_input(&transport, rx_buf, (size_t)n);

        for (;;) {
            uint8_t ptype = 0;
            const uint8_t *payload = NULL;
            size_t plen = 0;
            int r = ssh_transport_get_packet(&transport, &ptype, &payload, &plen);
            if (r != 0) break;

            if (ptype == SSH_MSG_KEXINIT) {
                int prc = kex_parse_kexinit(&transport.kex, payload, plen);
                if (prc == 0) {
                    transport.strict_kex = transport.kex.strict_kex;
                }
            } else if (ptype == SSH_MSG_KEX_ECDH_INIT) {
                /* Ephemeral Curve25519 Key Exchange */
                (void)kex_start_ecdh(&transport.kex);
                uint8_t ex_hash[32];
                (void)kex_process_ecdh_init(&transport.kex, payload, plen, host_pub,
                                            client_ver, SSHD_SERVER_VERSION_TRIM, ex_hash);

                /* Request signature from monitor via IPC */
                (void)monitor_send_msg(mon_out_fd, MONITOR_REQ_SIGN_HASH, (uint8_t)session_id, ex_hash, 32);
                monitor_hdr_t resp_hdr;
                static uint8_t s_worker_resp_payload[MONITOR_MAX_PAYLOAD];
                int mon_rc = monitor_recv_msg(mon_in_fd, &resp_hdr, s_worker_resp_payload, sizeof(s_worker_resp_payload));
                if (mon_rc == 0 && resp_hdr.msg_type == MONITOR_RESP_SIGN_HASH && resp_hdr.payload_len >= 68) {
                    uint32_t status = *(uint32_t *)s_worker_resp_payload;
                    if (status == MONITOR_STATUS_OK) {
                        memcpy(host_sig, s_worker_resp_payload + 4, 64);
                        host_sig_obtained = true;
                    }
                }

                if (host_sig_obtained) {
                    sshbuf_t ecdh_reply;
                    sshbuf_init(&ecdh_reply, 512, 4096);
                    (void)kex_build_ecdh_reply(&transport.kex, host_pub, host_sig, &ecdh_reply);
                    sshbuf_t reply_wire;
                    sshbuf_init(&reply_wire, 512, 4096);
                    (void)ssh_transport_send_packet(&transport, SSH_MSG_KEX_ECDH_REPLY,
                                                   sshbuf_ptr(&ecdh_reply) + 1, sshbuf_len(&ecdh_reply) - 1, &reply_wire);
                    (void)ssh_transport_send_packet(&transport, SSH_MSG_NEWKEYS, NULL, 0, &reply_wire);
                    (void)SSHD_SYSCALL(SYS_SEND, (uintptr_t)client_fd, (uintptr_t)sshbuf_ptr(&reply_wire), sshbuf_len(&reply_wire), 0, 0, 0);

                    sshbuf_free(&ecdh_reply);
                    sshbuf_free(&reply_wire);
                }
            } else if (ptype == SSH_MSG_NEWKEYS) {
                (void)ssh_transport_apply_newkeys(&transport, false);
                kex_complete = true;
                break;
            }
        }
    }

    if (!kex_complete) {
        ssh_transport_cleanup(&transport);
        return 1;
    }

    /* Step 5: SSH User Authentication Protocol */
    static ssh_auth_ctx_t auth_ctx;
    auth_init(&auth_ctx, transport.kex.session_id, (uint8_t)session_id, mon_in_fd, mon_out_fd);

    bool authenticated = false;
    sshbuf_t auth_reply;
    sshbuf_init(&auth_reply, 256, 4096);

    while (!authenticated && !g_terminate) {
        uint8_t ptype = 0;
        const uint8_t *payload = NULL;
        size_t plen = 0;
        int r = sshd_receive_packet(&transport, client_fd, rx_buf, sizeof(rx_buf),
                                    &ptype, &payload, &plen);
        if (r != 0) break;

        sshbuf_reset(&auth_reply);
        if (ptype == SSH_MSG_SERVICE_REQUEST) {
            (void)auth_process_service_request(&auth_ctx, payload, plen, &auth_reply);
        } else if (ptype == SSH_MSG_USERAUTH_REQUEST) {
            (void)auth_process_request(&auth_ctx, payload, plen, &auth_reply);
            if (auth_ctx.authenticated) {
                authenticated = true;
            }
        }

        if (sshbuf_len(&auth_reply) > 0) {
            uint8_t resp_type = 0;
            (void)sshbuf_get_u8(&auth_reply, &resp_type);
            sshbuf_t out_wire;
            sshbuf_init(&out_wire, 256, 4096);
            (void)ssh_transport_send_packet(&transport, resp_type,
                                           sshbuf_ptr(&auth_reply), sshbuf_len(&auth_reply),
                                           &out_wire);
            (void)SSHD_SYSCALL(SYS_SEND, (uintptr_t)client_fd, (uintptr_t)sshbuf_ptr(&out_wire), sshbuf_len(&out_wire), 0, 0, 0);
            sshbuf_free(&out_wire);
        }
        if (authenticated) break;
    }
    sshbuf_free(&auth_reply);

    if (!authenticated) {
        auth_cleanup(&auth_ctx);
        ssh_transport_cleanup(&transport);
        return 1;
    }

    /* Transition worker credentials to authenticated user */
    (void)SSHD_SYSCALL(SYS_SETGROUPS, auth_ctx.ngroups, (uintptr_t)auth_ctx.groups, 0, 0, 0, 0);
    (void)SSHD_SYSCALL(SYS_SETRESGID, auth_ctx.gid, auth_ctx.gid, auth_ctx.gid, 0, 0, 0);
    (void)SSHD_SYSCALL(SYS_SETRESUID, auth_ctx.uid, auth_ctx.uid, auth_ctx.uid, 0, 0, 0);
    (void)SSHD_SYSCALL(SYS_CAPSET, 0, 0, 0, 0, 0, 0);

    /* Step 6: Session Channels, PTY allocation, and Interactive Duplex Loop */
    sshbuf_t chan_reply;
    sshbuf_init(&chan_reply, 512, 16384);
    bool shell_spawned = false;
    int exit_drain_ticks = 0;

    for (;;) {
        if (g_terminate) break;

        /* A. Non-blocking drain from network socket */
        long n = SSHD_SYSCALL(SYS_RECV, (uintptr_t)client_fd, (uintptr_t)rx_buf, sizeof(rx_buf), NET_MSG_DONTWAIT, 0, 0);
        if (n > 0) {
            (void)ssh_transport_feed_input(&transport, rx_buf, (size_t)n);
            for (;;) {
                uint8_t ptype = 0;
                const uint8_t *payload = NULL;
                size_t plen = 0;
                int r = ssh_transport_get_packet(&transport, &ptype, &payload, &plen);
                if (r != 0) break;

                sshbuf_reset(&chan_reply);
                (void)channel_process_packet(&chan_mgr, ptype, payload, plen, &chan_reply);

                /* Check for window change */
                ssh_channel_t *c = channel_lookup_local(&chan_mgr, 0);
                if (c && c->pty_requested && shell_spawned) {
                    (void)pty_bridge_set_winsize(&bridge, (uint16_t)c->pty_params.cols, (uint16_t)c->pty_params.rows);
                }

                /* Check if shell or exec request was received */
                if (c && (c->shell_requested || c->exec_requested) && !shell_spawned) {
                    (void)pty_bridge_open(&bridge);
                    (void)pty_bridge_set_winsize(&bridge, (uint16_t)c->pty_params.cols, (uint16_t)c->pty_params.rows);

                    static char s_env_home[144];
                    static char s_env_user[48];
                    static char s_env_logname[48];
                    static char s_env_shell[144];

                    size_t k = 0;
                    while (k < 5 && "HOME="[k]) { s_env_home[k] = "HOME="[k]; k++; }
                    for (size_t i = 0; auth_ctx.home[i] && k + 1 < sizeof(s_env_home); i++) s_env_home[k++] = auth_ctx.home[i];
                    s_env_home[k] = '\0';

                    k = 0;
                    while (k < 5 && "USER="[k]) { s_env_user[k] = "USER="[k]; k++; }
                    for (size_t i = 0; auth_ctx.username[i] && k + 1 < sizeof(s_env_user); i++) s_env_user[k++] = auth_ctx.username[i];
                    s_env_user[k] = '\0';

                    k = 0;
                    while (k < 8 && "LOGNAME="[k]) { s_env_logname[k] = "LOGNAME="[k]; k++; }
                    for (size_t i = 0; auth_ctx.username[i] && k + 1 < sizeof(s_env_logname); i++) s_env_logname[k++] = auth_ctx.username[i];
                    s_env_logname[k] = '\0';

                    k = 0;
                    while (k < 6 && "SHELL="[k]) { s_env_shell[k] = "SHELL="[k]; k++; }
                    for (size_t i = 0; auth_ctx.shell[i] && k + 1 < sizeof(s_env_shell); i++) s_env_shell[k++] = auth_ctx.shell[i];
                    s_env_shell[k] = '\0';

                    char *shell_argv[] = { (char *)auth_ctx.shell, NULL };
                    const char *shell_envp[] = {
                        s_env_home, s_env_user, s_env_logname, s_env_shell,
                        "PATH=/bin:/sbin", "TERM=xterm-256color", NULL
                    };
                    (void)pty_bridge_spawn(&bridge, auth_ctx.shell, shell_argv, (char **)shell_envp, auth_ctx.home);
                    shell_spawned = true;

                    if (c->exec_requested) {
                        size_t clen = strlen(c->exec_cmd);
                        (void)sshbuf_put_raw(&c->in_data, (const uint8_t *)c->exec_cmd, clen);
                        (void)sshbuf_put_raw(&c->in_data, (const uint8_t *)"\nexit\n", 6);
                    }

                    /* Close monitor IPC pipes now that session shell is spawned */
                    (void)SSHD_SYSCALL(SYS_CLOSE, (uintptr_t)mon_in_fd, 0, 0, 0, 0, 0);
                    (void)SSHD_SYSCALL(SYS_CLOSE, (uintptr_t)mon_out_fd, 0, 0, 0, 0, 0);
                    mon_in_fd = -1;
                    mon_out_fd = -1;
                }

                /* Transmit any channel replies */
                if (sshbuf_len(&chan_reply) > 0) {
                    if (send_channel_replies(&transport, client_fd, &chan_reply) != 0) goto session_cleanup;
                }
            }
        } else if (n == 0 || (n < 0 && n != SYSCALL_EAGAIN && n != -11)) {
            /* Client TCP EOF or broken link / error */
            break;
        }

        /* B. Non-blocking PTY bridge step */
        bool pty_active = false;
        if (shell_spawned) {
            sshbuf_reset(&chan_reply);
            (void)pty_bridge_step(&bridge, &chan_mgr, &chan_reply, &pty_active);

            if (sshbuf_len(&chan_reply) > 0) {
                if (send_channel_replies(&transport, client_fd, &chan_reply) != 0) goto session_cleanup;
            }

            ssh_channel_t *ch0 = channel_lookup_local(&chan_mgr, 0);
            if (!ch0 || ch0->state == SSH_CHANNEL_STATE_CLOSED) {
                break;
            }
            if (bridge.child_exited) {
                exit_drain_ticks++;
                if (exit_drain_ticks > 15) {
                    break;
                }
            }
        }

        /* C. Bounded-poll sleep (20ms) if idle */
        if (n <= 0 && !pty_active) {
            pty_bridge_sleep_bounded(20);
        }
    }

session_cleanup:
    /* Transmit RFC 4253 SSH_MSG_DISCONNECT packet before socket close */
    if (client_fd >= 0) {
        sshd_send_disconnect(&transport,client_fd);
        (void)SSHD_SYSCALL(SYS_SHUTDOWN, (uintptr_t)client_fd, 1 /* SHUT_WR */, 0, 0, 0, 0);
    }

    /* Teardown */
    sshbuf_free(&chan_reply);
    pty_bridge_cleanup(&bridge);
    channel_mgr_cleanup(&chan_mgr);
    auth_cleanup(&auth_ctx);
    ssh_transport_cleanup(&transport);

    (void)SSHD_SYSCALL(SYS_CLOSE, (uintptr_t)client_fd, 0, 0, 0, 0, 0);
    if (mon_in_fd >= 0) (void)SSHD_SYSCALL(SYS_CLOSE, (uintptr_t)mon_in_fd, 0, 0, 0, 0, 0);
    if (mon_out_fd >= 0) (void)SSHD_SYSCALL(SYS_CLOSE, (uintptr_t)mon_out_fd, 0, 0, 0, 0, 0);

    return 0;
}

/* Privileged Monitor Daemon: listens on port 22 and coordinates workers */
static int sshd_monitor_run_loop(const sshd_config_t *cfg, const uint8_t host_secret[64],
                                 const userdb_t *db, long sock_fd) {
    (void)cfg;
    static monitor_ctx_t mon_ctx;
    monitor_init(&mon_ctx, host_secret);

    /* Use prepared user database */
    if (db) {
        mon_ctx.db = *db;
        mon_ctx.db_loaded = true;
    }

    /* Setup signal handlers for clean shutdown */
    signal_action_t sa = { .handler = (uintptr_t)on_sigterm, .mask = 0, .flags = 0, .reserved = 0 };
    (void)SSHD_SYSCALL(SYS_SIGACTION, SIGTERM, (uintptr_t)&sa, 0, 0, 0, 0);
    (void)SSHD_SYSCALL(SYS_SIGACTION, SIGINT, (uintptr_t)&sa, 0, 0, 0, 0);

    sshd_write_err("Listening on port 22 (SSH-2.0-FortressSSH_0.1)");

    static sshd_worker_slot_t slots[SSHD_MAX_CONCURRENT_SESSIONS];
    memset(slots, 0, sizeof(slots));

    while (!g_terminate) {
        /* 1. Reap terminated workers */
        for (int i = 0; i < SSHD_MAX_CONCURRENT_SESSIONS; i++) {
            if (slots[i].active) {
                uint64_t status = 0;
                long w = SSHD_SYSCALL(SYS_WAITPID, (uintptr_t)slots[i].pid, (uintptr_t)&status, WNOHANG, 0, 0, 0);
                if (w == slots[i].pid) {
                    slots[i].active = false;
                    (void)SSHD_SYSCALL(SYS_CLOSE, (uintptr_t)slots[i].p_mon_in, 0, 0, 0, 0, 0);
                    (void)SSHD_SYSCALL(SYS_CLOSE, (uintptr_t)slots[i].p_mon_out, 0, 0, 0, 0, 0);
                    slots[i].p_mon_in = -1;
                    slots[i].p_mon_out = -1;
                    memset(&mon_ctx.conns[i], 0, sizeof(mon_ctx.conns[i]));
                    mon_ctx.conns[i].session_id = (uint8_t)i;
                }
            }
        }

        /* 2. Service pending IPC requests from active workers */
        for (int i = 0; i < SSHD_MAX_CONCURRENT_SESSIONS; i++) {
            if (slots[i].active && slots[i].p_mon_in >= 0) {
                monitor_hdr_t req_hdr;
                static uint8_t s_req_payload[MONITOR_MAX_PAYLOAD];
                if (monitor_recv_msg(slots[i].p_mon_in, &req_hdr, s_req_payload, sizeof(s_req_payload)) == 0) {
                    uint8_t resp_type = 0;
                    static uint8_t s_resp_payload[MONITOR_MAX_PAYLOAD];
                    uint16_t resp_len = 0;
                    (void)monitor_handle_request(&mon_ctx, &req_hdr, s_req_payload,
                                                &resp_type, s_resp_payload, &resp_len, sizeof(s_resp_payload));
                    (void)monitor_send_msg(slots[i].p_mon_out, resp_type, req_hdr.session_id, s_resp_payload, resp_len);
                }
            }
        }

        /* 3. Accept incoming TCP connection (non-blocking) */
        long client_fd = SSHD_SYSCALL(SYS_ACCEPT, (uintptr_t)sock_fd, 0, 0, NET_SOCK_NONBLOCK, 0, 0);
        if (client_fd >= 0) {
            /* Enforce global admission cap before worker spawn */
            int free_slot = sshd_admission_find_slot(slots);

            if (free_slot < 0) {
                /* Exceeded max concurrent sessions (2 globally across all IPs) */
                sshd_write_err("Connection rejected: max concurrent sessions reached.");
                sshd_reject_connection_capacity(client_fd, SSHD_SERVER_VERSION);
                continue;
            }

            memset(&mon_ctx.conns[free_slot], 0, sizeof(mon_ctx.conns[free_slot]));
            mon_ctx.conns[free_slot].session_id = (uint8_t)free_slot;
            mon_ctx.conns[free_slot].active = true;

            /* Create IPC pipes connecting monitor and worker */
            int p_worker_to_mon[2];
            int p_mon_to_worker[2];
            if (SSHD_SYSCALL(SYS_PIPE, (uintptr_t)p_worker_to_mon, 0, 0, 0, 0, 0) != 0 ||
                SSHD_SYSCALL(SYS_PIPE, (uintptr_t)p_mon_to_worker, 0, 0, 0, 0, 0) != 0) {
                (void)SSHD_SYSCALL(SYS_CLOSE, (uintptr_t)client_fd, 0, 0, 0, 0, 0);
                continue;
            }

            /* Set monitor's read endpoint non-blocking */
            (void)SSHD_SYSCALL(SYS_FCNTL, (uintptr_t)p_worker_to_mon[0], F_SETFL, VFS_O_NONBLOCK, 0, 0, 0);

            /* Prepare worker spawn arguments: /sbin/sshd --worker <slot> <pub_hex> */
            static char s_host_pub_hex[65];
            static const char hex_chars[] = "0123456789abcdef";
            for (int k = 0; k < 32; k++) {
                s_host_pub_hex[k * 2]     = hex_chars[(mon_ctx.host_public[k] >> 4) & 0x0f];
                s_host_pub_hex[k * 2 + 1] = hex_chars[mon_ctx.host_public[k] & 0x0f];
            }
            s_host_pub_hex[64] = '\0';

            static char s_slot_str[4];
            s_slot_str[0] = (char)('0' + free_slot);
            s_slot_str[1] = '\0';
            static const char *s_worker_argv[] = { "/sbin/sshd", "--worker", s_slot_str, s_host_pub_hex, NULL };

            /* Map descriptors into worker child:
             * fd 3 = client_fd
             * fd 4 = p_mon_to_worker[0] (worker reads monitor replies)
             * fd 5 = p_worker_to_mon[1] (worker writes requests to monitor)
             */
            static spawn_fd_action_t s_actions[3];
            memset(s_actions, 0, sizeof(s_actions));
            s_actions[0] = (spawn_fd_action_t){ .type = SPAWN_FD_ACTION_DUP2, .dst_fd = 3, .src_fd = (int32_t)client_fd };
            s_actions[1] = (spawn_fd_action_t){ .type = SPAWN_FD_ACTION_DUP2, .dst_fd = 4, .src_fd = (int32_t)p_mon_to_worker[0] };
            s_actions[2] = (spawn_fd_action_t){ .type = SPAWN_FD_ACTION_DUP2, .dst_fd = 5, .src_fd = (int32_t)p_worker_to_mon[1] };

            static spawn_opts_t s_opts;
            memset(&s_opts, 0, sizeof(s_opts));
            s_opts.size = sizeof(s_opts);
            s_opts.version = 2;
            s_opts.flags = SPAWN_SETPGROUP;
            s_opts.argv = (uint64_t)(uintptr_t)s_worker_argv;
            s_opts.fd_actions = (uint64_t)(uintptr_t)s_actions;
            s_opts.action_count = 3;

            long worker_pid = SSHD_SYSCALL(SYS_SPAWN_EXT, (uintptr_t)"/sbin/sshd", (uintptr_t)&s_opts, sizeof(s_opts), 0, 0, 0);
            if (worker_pid > 0) {
                slots[free_slot].active = true;
                slots[free_slot].pid = worker_pid;
                slots[free_slot].p_mon_in = p_worker_to_mon[0];
                slots[free_slot].p_mon_out = p_mon_to_worker[1];
                mon_ctx.conns[free_slot].session_id = (uint8_t)free_slot;
                mon_ctx.conns[free_slot].active = true;
            }

            /* Close endpoints now owned by child */
            (void)SSHD_SYSCALL(SYS_CLOSE, (uintptr_t)client_fd, 0, 0, 0, 0, 0);
            (void)SSHD_SYSCALL(SYS_CLOSE, (uintptr_t)p_mon_to_worker[0], 0, 0, 0, 0, 0);
            (void)SSHD_SYSCALL(SYS_CLOSE, (uintptr_t)p_worker_to_mon[1], 0, 0, 0, 0, 0);
        }

        /* Bounded sleep to avoid tight loop on accept interrupt */
        pty_bridge_sleep_bounded(20);
    }

    /* Shutdown: terminate active workers and close listening socket via pinned teardown */
    sshd_teardown_ops_t td_ops = {
        .kill = (int (*)(int64_t, int))sys_kill_wrapper,
        .waitpid = (int64_t (*)(int64_t, uint64_t *, int))sys_waitpid_wrapper,
        .close = (int (*)(int))sys_close_wrapper,
        .sleep_ms = pty_bridge_sleep_bounded
    };
    (void)sshd_daemon_teardown(slots, sock_fd, SSHD_TEARDOWN_TIMEOUT_MS, &td_ops);
    sshd_write_err("Daemon shut down cleanly.");
    return 0;
}

int sshd_main(int argc, char **argv) {
    /* Check for --worker mode */
    if (argc >= 3 && argv && strcmp(argv[1], "--worker") == 0) {
        int sid = argv[2][0] - '0';
        if (sid < 0 || sid > 1) sid = 0;
        const char *pub_hex = (argc >= 4) ? argv[3] : NULL;
        return sshd_worker_run(sid, 3, 4, 5, pub_hex);
    }

    /* Monitor mode: execute pinned boot readiness gate sequence */
    const char *cfg_path = NULL;
    bool foreground = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            cfg_path = argv[++i];
        } else if (strcmp(argv[i], "-D") == 0) {
            foreground = true;
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            /* Handled below */
        }
    }

    signal_action_t sa = { .handler = (uintptr_t)on_sigterm, .mask = 0, .flags = 0, .reserved = 0 };
    (void)SSHD_SYSCALL(SYS_SIGACTION, SIGTERM, (uintptr_t)&sa, 0, 0, 0, 0);
    (void)SSHD_SYSCALL(SYS_SIGACTION, SIGINT, (uintptr_t)&sa, 0, 0, 0, 0);

    static sshd_config_t cfg;
    static uint8_t s_main_host_secret[64];
    static userdb_t db;
    long sock_fd = -1;
    static char diag[256];
    memset(diag, 0, sizeof(diag));
    bool is_disabled = false;

    int rc = sshd_check_readiness(cfg_path, NULL, &cfg, s_main_host_secret, &db, &sock_fd,
                                  diag, sizeof(diag), &is_disabled);

    if (is_disabled) {
        sshd_write_err(diag[0] ? diag : "Service disabled in configuration.");
        return 0;
    }

    if (rc != 0) {
        if (foreground && (strstr(diag, "bind failed") != NULL ||
                           strstr(diag, "listen") != NULL ||
                           strstr(diag, "Failed to listen") != NULL)) {
            sshd_write_err("sshd: network offline; waiting for network link carrier...");
            while (rc != 0 && !g_terminate) {
                pty_bridge_sleep_bounded(2000);
                if (g_terminate) {
                    return 0;
                }
                memset(diag, 0, sizeof(diag));
                rc = sshd_check_readiness(cfg_path, NULL, &cfg, s_main_host_secret, &db, &sock_fd,
                                          diag, sizeof(diag), &is_disabled);
                if (is_disabled) {
                    sshd_write_err(diag[0] ? diag : "Service disabled in configuration.");
                    return 0;
                }
                if (rc != 0 && !(strstr(diag, "bind failed") != NULL ||
                                 strstr(diag, "listen") != NULL ||
                                 strstr(diag, "Failed to listen") != NULL)) {
                    sshd_write_err(diag[0] ? diag : "SSH daemon boot readiness check failed.");
                    return 1;
                }
            }
            if (g_terminate) {
                return 0;
            }
            if (rc == 0) {
                sshd_write_err("sshd: network link online; bound port 22");
            }
        } else {
            sshd_write_err(diag[0] ? diag : "SSH daemon boot readiness check failed.");
            return 1;
        }
    }

    if (foreground) {
        long rfd = SSHD_SYSCALL(SYS_OPEN, (uintptr_t)"/run/watchtower/sshd.ready",
                                VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC, 0644, 0, 0, 0);
        if (rfd >= 0) {
            (void)SSHD_SYSCALL(SYS_CLOSE, (uintptr_t)rfd, 0, 0, 0, 0, 0);
        }
    }

    int run_rc = sshd_monitor_run_loop(&cfg, s_main_host_secret, &db, sock_fd);
    crypto_wipe(s_main_host_secret, sizeof(s_main_host_secret));

    if (foreground) {
        (void)SSHD_SYSCALL(SYS_UNLINK, (uintptr_t)"/run/watchtower/sshd.ready", 0, 0, 0, 0, 0);
    }
    return run_rc;
}
