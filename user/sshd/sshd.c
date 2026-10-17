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
#ifdef __FORTRESS__
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
        (void)SSHD_SYSCALL(SYS_WRITE, 2, (uintptr_t)"[sshd] ", 7, 0, 0, 0);
        (void)SSHD_SYSCALL(SYS_WRITE, 2, (uintptr_t)msg, len, 0, 0, 0);
        (void)SSHD_SYSCALL(SYS_WRITE, 2, (uintptr_t)"\n", 1, 0, 0, 0);
    }
}

static volatile int g_terminate = 0;
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



/* Worker Process: runs the connection protocol under dropped capabilities */
static int sshd_worker_run(int session_id, int client_fd, int mon_in_fd, int mon_out_fd) {
    /* Setup signal handlers so SIGTERM breaks worker poll loop and runs clean pty_bridge_cleanup() */
    signal_action_t sa = { .handler = (uintptr_t)on_sigterm, .mask = 0, .flags = 0, .reserved = 0 };
    (void)SSHD_SYSCALL(SYS_SIGACTION, SIGTERM, (uintptr_t)&sa, 0, 0, 0, 0);
    (void)SSHD_SYSCALL(SYS_SIGACTION, SIGINT, (uintptr_t)&sa, 0, 0, 0, 0);

    /* Step 1: Drop all capabilities immediately */
    (void)SSHD_SYSCALL(SYS_CAPSET, 0, 0, 0, 0, 0, 0);

    /* Step 2: Initialize protocol state */
    ssh_transport_t transport;
    if (ssh_transport_init(&transport) != 0) return 1;

    ssh_channel_mgr_t chan_mgr;
    channel_mgr_init(&chan_mgr);

    pty_bridge_t bridge;
    pty_bridge_init(&bridge, 0, 80, 24);

    /* Step 3: Identification exchange */
    /* Send server version */
    (void)SSHD_SYSCALL(SYS_SEND, (uintptr_t)client_fd, (uintptr_t)SSHD_SERVER_VERSION, strlen(SSHD_SERVER_VERSION), 0, 0, 0);

    /* Read client version */
    char client_ver[256];
    size_t cv_len = 0;
    while (cv_len + 1 < sizeof(client_ver)) {
        char ch = 0;
        long n = SSHD_SYSCALL(SYS_RECV, (uintptr_t)client_fd, (uintptr_t)&ch, 1, 0, 0, 0);
        if (n <= 0) break;
        client_ver[cv_len++] = ch;
        if (ch == '\n') break;
    }
    client_ver[cv_len] = '\0';
    while (cv_len > 0 && (client_ver[cv_len - 1] == '\r' || client_ver[cv_len - 1] == '\n')) {
        client_ver[--cv_len] = '\0';
    }

    /* Step 4: KEXINIT exchange */
    uint8_t cookie[16];
    (void)SSHD_SYSCALL(SYS_GETRANDOM, (uintptr_t)cookie, sizeof(cookie), 0, 0, 0, 0);

    sshbuf_t kexinit_wire;
    if (sshbuf_init(&kexinit_wire, 512, 4096) != 0) return 1;
    (void)ssh_transport_start_rekey(&transport, cookie, &kexinit_wire);
    (void)SSHD_SYSCALL(SYS_SEND, (uintptr_t)client_fd, (uintptr_t)sshbuf_ptr(&kexinit_wire), sshbuf_len(&kexinit_wire), 0, 0, 0);
    sshbuf_free(&kexinit_wire);

    /* Read client KEXINIT & KEX_ECDH_INIT */
    uint8_t rx_buf[4096];
    bool kex_complete = false;
    uint8_t host_pub[32];
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
                (void)kex_parse_kexinit(&transport.kex, payload, plen);
            } else if (ptype == SSH_MSG_KEX_ECDH_INIT) {
                /* Ephemeral Curve25519 Key Exchange */
                (void)kex_start_ecdh(&transport.kex);
                uint8_t ex_hash[32];
                (void)kex_process_ecdh_init(&transport.kex, payload, plen, host_pub,
                                            client_ver, SSHD_SERVER_VERSION_TRIM, ex_hash);

                /* Request signature from monitor via IPC */
                (void)monitor_send_msg(mon_out_fd, MONITOR_REQ_SIGN_HASH, (uint8_t)session_id, ex_hash, 32);
                monitor_hdr_t resp_hdr;
                uint8_t resp_payload[MONITOR_MAX_PAYLOAD];
                if (monitor_recv_msg(mon_in_fd, &resp_hdr, resp_payload, sizeof(resp_payload)) == 0 &&
                    resp_hdr.msg_type == MONITOR_RESP_SIGN_HASH && resp_hdr.payload_len >= 96) {
                    memcpy(host_pub, resp_payload, 32);
                    memcpy(host_sig, resp_payload + 32, 64);
                    host_sig_obtained = true;
                }

                if (host_sig_obtained) {
                    sshbuf_t ecdh_reply;
                    sshbuf_init(&ecdh_reply, 512, 4096);
                    (void)kex_build_ecdh_reply(&transport.kex, host_pub, host_sig, &ecdh_reply);
                    sshbuf_t reply_wire;
                    sshbuf_init(&reply_wire, 512, 4096);
                    (void)ssh_transport_send_packet(&transport, SSH_MSG_KEX_ECDH_REPLY,
                                                   sshbuf_ptr(&ecdh_reply), sshbuf_len(&ecdh_reply), &reply_wire);
                    (void)SSHD_SYSCALL(SYS_SEND, (uintptr_t)client_fd, (uintptr_t)sshbuf_ptr(&reply_wire), sshbuf_len(&reply_wire), 0, 0, 0);

                    /* Send NEWKEYS */
                    sshbuf_reset(&reply_wire);
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
    ssh_auth_ctx_t auth_ctx;
    auth_init(&auth_ctx, transport.kex.session_id, (uint8_t)session_id, mon_in_fd, mon_out_fd);

    bool authenticated = false;
    sshbuf_t auth_reply;
    sshbuf_init(&auth_reply, 256, 4096);

    while (!authenticated && !g_terminate) {
        long n = SSHD_SYSCALL(SYS_RECV, (uintptr_t)client_fd, (uintptr_t)rx_buf, sizeof(rx_buf), 0, 0, 0);
        if (n <= 0) break;
        (void)ssh_transport_feed_input(&transport, rx_buf, (size_t)n);

        for (;;) {
            uint8_t ptype = 0;
            const uint8_t *payload = NULL;
            size_t plen = 0;
            int r = ssh_transport_get_packet(&transport, &ptype, &payload, &plen);
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
                                               sshbuf_ptr(&auth_reply) + 1, sshbuf_len(&auth_reply) - 1,
                                               &out_wire);
                (void)SSHD_SYSCALL(SYS_SEND, (uintptr_t)client_fd, (uintptr_t)sshbuf_ptr(&out_wire), sshbuf_len(&out_wire), 0, 0, 0);
                sshbuf_free(&out_wire);
            }
            if (authenticated) break;
        }
    }
    sshbuf_free(&auth_reply);

    if (!authenticated) {
        auth_cleanup(&auth_ctx);
        ssh_transport_cleanup(&transport);
        return 1;
    }

    /* Step 6: Session Channels, PTY allocation, and Interactive Duplex Loop */
    sshbuf_t chan_reply;
    sshbuf_init(&chan_reply, 512, 16384);
    bool shell_spawned = false;

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

                /* Check if shell request was received */
                if (c && c->shell_requested && !shell_spawned) {
                    (void)pty_bridge_open(&bridge);
                    (void)pty_bridge_set_winsize(&bridge, (uint16_t)c->pty_params.cols, (uint16_t)c->pty_params.rows);

                    char *shell_argv[] = { (char *)auth_ctx.shell, NULL };
                    char *shell_envp[] = { "TERM=xterm-256color", NULL };
                    (void)pty_bridge_spawn(&bridge, auth_ctx.shell, shell_argv, shell_envp, auth_ctx.home);
                    shell_spawned = true;

                    /* Close monitor IPC pipes now that session shell is spawned */
                    (void)SSHD_SYSCALL(SYS_CLOSE, (uintptr_t)mon_in_fd, 0, 0, 0, 0, 0);
                    (void)SSHD_SYSCALL(SYS_CLOSE, (uintptr_t)mon_out_fd, 0, 0, 0, 0, 0);
                    mon_in_fd = -1;
                    mon_out_fd = -1;
                }

                /* Transmit any channel replies */
                if (sshbuf_len(&chan_reply) > 0) {
                    while (sshbuf_len(&chan_reply) > 0) {
                        uint8_t rtype = 0;
                        (void)sshbuf_get_u8(&chan_reply, &rtype);
                        sshbuf_t out_wire;
                        sshbuf_init(&out_wire, 512, 4096);
                        (void)ssh_transport_send_packet(&transport, rtype,
                                                       sshbuf_ptr(&chan_reply) + 1, sshbuf_len(&chan_reply) - 1,
                                                       &out_wire);
                        (void)SSHD_SYSCALL(SYS_SEND, (uintptr_t)client_fd, (uintptr_t)sshbuf_ptr(&out_wire), sshbuf_len(&out_wire), 0, 0, 0);
                        sshbuf_free(&out_wire);
                        break;
                    }
                }
            }
        } else if (n == 0) {
            /* Client TCP EOF */
            break;
        }

        /* B. Non-blocking PTY bridge step */
        bool pty_active = false;
        if (shell_spawned) {
            sshbuf_reset(&chan_reply);
            (void)pty_bridge_step(&bridge, &chan_mgr, &chan_reply, &pty_active);

            if (sshbuf_len(&chan_reply) > 0) {
                while (sshbuf_len(&chan_reply) > 0) {
                    uint8_t rtype = 0;
                    (void)sshbuf_get_u8(&chan_reply, &rtype);
                    sshbuf_t out_wire;
                    sshbuf_init(&out_wire, 512, 4096);
                    (void)ssh_transport_send_packet(&transport, rtype,
                                                   sshbuf_ptr(&chan_reply) + 1, sshbuf_len(&chan_reply) - 1,
                                                   &out_wire);
                    (void)SSHD_SYSCALL(SYS_SEND, (uintptr_t)client_fd, (uintptr_t)sshbuf_ptr(&out_wire), sshbuf_len(&out_wire), 0, 0, 0);
                    sshbuf_free(&out_wire);
                    break;
                }
            }

            if (bridge.child_exited) {
                /* Shell process has completed */
                break;
            }
        }

        /* C. Bounded-poll sleep (20ms) if idle */
        if (n <= 0 && !pty_active) {
            pty_bridge_sleep_bounded(20);
        }
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
    monitor_ctx_t mon_ctx;
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

    sshd_worker_slot_t slots[SSHD_MAX_CONCURRENT_SESSIONS];
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
                    mon_ctx.conns[i].active = false;
                }
            }
        }

        /* 2. Service pending IPC requests from active workers */
        for (int i = 0; i < SSHD_MAX_CONCURRENT_SESSIONS; i++) {
            if (slots[i].active && slots[i].p_mon_in >= 0) {
                monitor_hdr_t req_hdr;
                uint8_t req_payload[MONITOR_MAX_PAYLOAD];
                if (monitor_recv_msg(slots[i].p_mon_in, &req_hdr, req_payload, sizeof(req_payload)) == 0) {
                    uint8_t resp_type = 0;
                    uint8_t resp_payload[MONITOR_MAX_PAYLOAD];
                    uint16_t resp_len = 0;
                    (void)monitor_handle_request(&mon_ctx, &req_hdr, req_payload,
                                                &resp_type, resp_payload, &resp_len, sizeof(resp_payload));
                    (void)monitor_send_msg(slots[i].p_mon_out, resp_type, req_hdr.session_id, resp_payload, resp_len);
                }
            }
        }

        /* 3. Accept incoming TCP connection */
        net_sockaddr_in_t peer;
        uint32_t peer_len = sizeof(peer);
        long client_fd = SSHD_SYSCALL(SYS_ACCEPT, (uintptr_t)sock_fd, (uintptr_t)&peer, (uintptr_t)&peer_len, 0, 0, 0);
        if (client_fd >= 0) {
            /* Enforce global admission cap before worker spawn */
            int free_slot = sshd_admission_find_slot(slots);

            if (free_slot < 0) {
                /* Exceeded max concurrent sessions (2 globally across all IPs) */
                sshd_write_err("Connection rejected: max concurrent sessions reached.");
                sshd_reject_connection_capacity(client_fd, SSHD_SERVER_VERSION);
                continue;
            }

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

            /* Prepare worker spawn arguments: /sbin/sshd --worker <slot> */
            char slot_str[4];
            slot_str[0] = (char)('0' + free_slot);
            slot_str[1] = '\0';
            const char *worker_argv[] = { "/sbin/sshd", "--worker", slot_str, NULL };

            /* Map descriptors into worker child:
             * fd 3 = client_fd
             * fd 4 = p_mon_to_worker[0] (worker reads monitor replies)
             * fd 5 = p_worker_to_mon[1] (worker writes requests to monitor)
             */
            spawn_fd_action_t actions[3];
            memset(actions, 0, sizeof(actions));
            actions[0] = (spawn_fd_action_t){ .type = SPAWN_FD_ACTION_DUP2, .dst_fd = 3, .src_fd = (int32_t)client_fd };
            actions[1] = (spawn_fd_action_t){ .type = SPAWN_FD_ACTION_DUP2, .dst_fd = 4, .src_fd = (int32_t)p_mon_to_worker[0] };
            actions[2] = (spawn_fd_action_t){ .type = SPAWN_FD_ACTION_DUP2, .dst_fd = 5, .src_fd = (int32_t)p_worker_to_mon[1] };

            spawn_opts_t opts;
            memset(&opts, 0, sizeof(opts));
            opts.size = sizeof(opts);
            opts.version = 2;
            opts.flags = SPAWN_SETPGROUP;
            opts.argv = (uint64_t)(uintptr_t)worker_argv;
            opts.fd_actions = (uint64_t)(uintptr_t)actions;
            opts.action_count = 3;

            long worker_pid = SSHD_SYSCALL(SYS_SPAWN_EXT, (uintptr_t)"/sbin/sshd", (uintptr_t)&opts, sizeof(opts), 0, 0, 0);
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
        return sshd_worker_run(sid, 3, 4, 5);
    }

    /* Monitor mode: execute pinned boot readiness gate sequence */
    const char *cfg_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            cfg_path = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            /* Handled below */
        }
    }

    sshd_config_t cfg;
    uint8_t host_secret[64];
    userdb_t db;
    long sock_fd = -1;
    char diag[256] = {0};
    bool is_disabled = false;

    int rc = sshd_check_readiness(cfg_path, NULL, &cfg, host_secret, &db, &sock_fd,
                                  diag, sizeof(diag), &is_disabled);

    if (is_disabled) {
        sshd_write_err(diag[0] ? diag : "Service disabled in configuration.");
        return 0;
    }

    if (rc != 0) {
        sshd_write_err(diag[0] ? diag : "SSH daemon boot readiness check failed.");
        return 1;
    }

    int run_rc = sshd_monitor_run_loop(&cfg, host_secret, &db, sock_fd);
    crypto_wipe(host_secret, sizeof(host_secret));
    return run_rc;
}
