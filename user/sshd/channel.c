#include "channel.h"
#include "string.h"

void channel_mgr_init(ssh_channel_mgr_t *mgr) {
    if (!mgr) return;
    memset(mgr, 0, sizeof(*mgr));
    mgr->next_local_id = 0;
}

void channel_mgr_cleanup(ssh_channel_mgr_t *mgr) {
    if (!mgr) return;
    for (size_t i = 0; i < SSH_CHANNEL_MAX_CHANNELS; i++) {
        if (mgr->channels[i].state != SSH_CHANNEL_STATE_CLOSED) {
            sshbuf_free(&mgr->channels[i].in_data);
        }
    }
    memset(mgr, 0, sizeof(*mgr));
}

ssh_channel_t *channel_lookup_local(ssh_channel_mgr_t *mgr, uint32_t local_id) {
    if (!mgr) return NULL;
    for (size_t i = 0; i < SSH_CHANNEL_MAX_CHANNELS; i++) {
        if (mgr->channels[i].state != SSH_CHANNEL_STATE_CLOSED &&
            mgr->channels[i].local_id == local_id) {
            return &mgr->channels[i];
        }
    }
    return NULL;
}

ssh_channel_t *channel_lookup_remote(ssh_channel_mgr_t *mgr, uint32_t remote_id) {
    if (!mgr) return NULL;
    for (size_t i = 0; i < SSH_CHANNEL_MAX_CHANNELS; i++) {
        if (mgr->channels[i].state != SSH_CHANNEL_STATE_CLOSED &&
            mgr->channels[i].remote_id == remote_id) {
            return &mgr->channels[i];
        }
    }
    return NULL;
}

static ssh_channel_t *channel_alloc(ssh_channel_mgr_t *mgr) {
    if (!mgr) return NULL;
    for (size_t i = 0; i < SSH_CHANNEL_MAX_CHANNELS; i++) {
        if (mgr->channels[i].state == SSH_CHANNEL_STATE_CLOSED) {
            memset(&mgr->channels[i], 0, sizeof(ssh_channel_t));
            return &mgr->channels[i];
        }
    }
    return NULL;
}

static int send_open_failure(uint32_t remote_id, uint32_t reason, const char *desc, sshbuf_t *out) {
    if (sshbuf_put_u8(out, SSH_MSG_CHANNEL_OPEN_FAILURE) != 0) return SSH_ERR_NO_MEM;
    if (sshbuf_put_u32(out, remote_id) != 0) return SSH_ERR_NO_MEM;
    if (sshbuf_put_u32(out, reason) != 0) return SSH_ERR_NO_MEM;
    if (sshbuf_put_cstring(out, desc ? desc : "") != 0) return SSH_ERR_NO_MEM;
    if (sshbuf_put_cstring(out, "") != 0) return SSH_ERR_NO_MEM; /* language tag */
    return 0;
}

static int handle_channel_open(ssh_channel_mgr_t *mgr, sshbuf_t *in, sshbuf_t *out_reply) {
    const uint8_t *ctype_ptr = NULL;
    size_t ctype_len = 0;
    if (sshbuf_get_string_ptr(in, &ctype_ptr, &ctype_len) != 0) return SSH_ERR_INVALID_FORMAT;

    uint32_t sender_channel = 0;
    uint32_t initial_window = 0;
    uint32_t max_packet = 0;
    if (sshbuf_get_u32(in, &sender_channel) != 0 ||
        sshbuf_get_u32(in, &initial_window) != 0 ||
        sshbuf_get_u32(in, &max_packet) != 0) {
        return SSH_ERR_INVALID_FORMAT;
    }

    /* We only support "session" channels */
    if (ctype_len != 7 || memcmp(ctype_ptr, "session", 7) != 0) {
        return send_open_failure(sender_channel, SSH_OPEN_UNKNOWN_CHANNEL_TYPE,
                                 "Unsupported channel type", out_reply);
    }

    ssh_channel_t *c = channel_alloc(mgr);
    if (!c) {
        return send_open_failure(sender_channel, SSH_OPEN_RESOURCE_SHORTAGE,
                                 "Channel capacity exceeded", out_reply);
    }

    c->local_id = mgr->next_local_id++;
    c->remote_id = sender_channel;
    c->state = SSH_CHANNEL_STATE_OPEN;
    c->remote_window = initial_window;
    c->remote_maxpacket = max_packet ? max_packet : SSH_CHANNEL_DEFAULT_MAXPACKET;
    c->local_window = SSH_CHANNEL_DEFAULT_WINDOW;
    c->local_maxpacket = SSH_CHANNEL_DEFAULT_MAXPACKET;

    if (sshbuf_init(&c->in_data, 4096, 64 * 1024) != 0) {
        c->state = SSH_CHANNEL_STATE_CLOSED;
        return send_open_failure(sender_channel, SSH_OPEN_RESOURCE_SHORTAGE,
                                 "Buffer allocation failure", out_reply);
    }

    /* Build SSH_MSG_CHANNEL_OPEN_CONFIRMATION */
    if (sshbuf_put_u8(out_reply, SSH_MSG_CHANNEL_OPEN_CONFIRMATION) != 0) return SSH_ERR_NO_MEM;
    if (sshbuf_put_u32(out_reply, c->remote_id) != 0) return SSH_ERR_NO_MEM;
    if (sshbuf_put_u32(out_reply, c->local_id) != 0) return SSH_ERR_NO_MEM;
    if (sshbuf_put_u32(out_reply, c->local_window) != 0) return SSH_ERR_NO_MEM;
    if (sshbuf_put_u32(out_reply, c->local_maxpacket) != 0) return SSH_ERR_NO_MEM;

    return 0;
}

static int handle_window_adjust(ssh_channel_mgr_t *mgr, sshbuf_t *in) {
    uint32_t recipient_channel = 0;
    uint32_t bytes_to_add = 0;
    if (sshbuf_get_u32(in, &recipient_channel) != 0 ||
        sshbuf_get_u32(in, &bytes_to_add) != 0) {
        return SSH_ERR_INVALID_FORMAT;
    }

    ssh_channel_t *c = channel_lookup_local(mgr, recipient_channel);
    if (!c) return 0; /* Ignore window adjust on inactive or closed channel */

    /* Overflow check per RFC 4254 §5.2 */
    if (c->remote_window + bytes_to_add < c->remote_window) {
        return SSH_ERR_PROTOCOL_VIOLATION;
    }

    c->remote_window += bytes_to_add;
    return 0;
}

static int handle_channel_data(ssh_channel_mgr_t *mgr, sshbuf_t *in, sshbuf_t *out_reply) {
    uint32_t recipient_channel = 0;
    if (sshbuf_get_u32(in, &recipient_channel) != 0) return SSH_ERR_INVALID_FORMAT;

    const uint8_t *data_ptr = NULL;
    size_t data_len = 0;
    if (sshbuf_get_string_ptr(in, &data_ptr, &data_len) != 0) return SSH_ERR_INVALID_FORMAT;

    ssh_channel_t *c = channel_lookup_local(mgr, recipient_channel);
    if (!c || c->state == SSH_CHANNEL_STATE_CLOSED) return SSH_ERR_INVALID_ARG;

    if (c->state == SSH_CHANNEL_STATE_EOF_REMOTE ||
        c->state == SSH_CHANNEL_STATE_EOF_BOTH ||
        c->state == SSH_CHANNEL_STATE_CLOSE_RCVD) {
        return SSH_ERR_PROTOCOL_VIOLATION; /* Received data after EOF or CLOSE from remote */
    }

    /* Window boundary validation: RFC 4254 §5.2 */
    if (data_len > c->local_window) {
        return SSH_ERR_PROTOCOL_VIOLATION;
    }

    c->local_window -= (uint32_t)data_len;

    if (data_len > 0) {
        int r = sshbuf_put_raw(&c->in_data, data_ptr, data_len);
        if (r != 0) return r;
    }

    /* If local window falls below threshold, issue window adjust to top up */
    if (c->local_window < SSH_CHANNEL_MIN_WINDOW_ADJUST) {
        uint32_t adjust = SSH_CHANNEL_DEFAULT_WINDOW - c->local_window;
        c->local_window += adjust;

        if (sshbuf_put_u8(out_reply, SSH_MSG_CHANNEL_WINDOW_ADJUST) != 0) return SSH_ERR_NO_MEM;
        if (sshbuf_put_u32(out_reply, c->remote_id) != 0) return SSH_ERR_NO_MEM;
        if (sshbuf_put_u32(out_reply, adjust) != 0) return SSH_ERR_NO_MEM;
    }

    return 0;
}

static int handle_channel_extended_data(ssh_channel_mgr_t *mgr, sshbuf_t *in, sshbuf_t *out_reply) {
    uint32_t recipient_channel = 0;
    uint32_t type_code = 0;
    if (sshbuf_get_u32(in, &recipient_channel) != 0 ||
        sshbuf_get_u32(in, &type_code) != 0) {
        return SSH_ERR_INVALID_FORMAT;
    }

    const uint8_t *data_ptr = NULL;
    size_t data_len = 0;
    if (sshbuf_get_string_ptr(in, &data_ptr, &data_len) != 0) return SSH_ERR_INVALID_FORMAT;

    ssh_channel_t *c = channel_lookup_local(mgr, recipient_channel);
    if (!c || c->state == SSH_CHANNEL_STATE_CLOSED) return SSH_ERR_INVALID_ARG;

    if (data_len > c->local_window) {
        return SSH_ERR_PROTOCOL_VIOLATION;
    }
    c->local_window -= (uint32_t)data_len;

    /* Feed into inbound data */
    if (data_len > 0) {
        int r = sshbuf_put_raw(&c->in_data, data_ptr, data_len);
        if (r != 0) return r;
    }

    if (c->local_window < SSH_CHANNEL_MIN_WINDOW_ADJUST) {
        uint32_t adjust = SSH_CHANNEL_DEFAULT_WINDOW - c->local_window;
        c->local_window += adjust;

        if (sshbuf_put_u8(out_reply, SSH_MSG_CHANNEL_WINDOW_ADJUST) != 0) return SSH_ERR_NO_MEM;
        if (sshbuf_put_u32(out_reply, c->remote_id) != 0) return SSH_ERR_NO_MEM;
        if (sshbuf_put_u32(out_reply, adjust) != 0) return SSH_ERR_NO_MEM;
    }

    return 0;
}

static int handle_channel_eof(ssh_channel_mgr_t *mgr, sshbuf_t *in) {
    uint32_t recipient_channel = 0;
    if (sshbuf_get_u32(in, &recipient_channel) != 0) return SSH_ERR_INVALID_FORMAT;

    ssh_channel_t *c = channel_lookup_local(mgr, recipient_channel);
    if (!c) return 0;

    if (c->state == SSH_CHANNEL_STATE_OPEN) {
        c->state = SSH_CHANNEL_STATE_EOF_REMOTE;
    } else if (c->state == SSH_CHANNEL_STATE_EOF_LOCAL) {
        c->state = SSH_CHANNEL_STATE_EOF_BOTH;
    }

    return 0;
}

static int handle_channel_close(ssh_channel_mgr_t *mgr, sshbuf_t *in, sshbuf_t *out_reply) {
    uint32_t recipient_channel = 0;
    if (sshbuf_get_u32(in, &recipient_channel) != 0) return SSH_ERR_INVALID_FORMAT;

    ssh_channel_t *c = channel_lookup_local(mgr, recipient_channel);
    if (!c) return 0;

    if (c->state == SSH_CHANNEL_STATE_CLOSE_SENT) {
        /* We sent CLOSE first, client responded with CLOSE. Teardown complete. */
        sshbuf_free(&c->in_data);
        memset(c, 0, sizeof(*c));
    } else {
        /* Client sent CLOSE first. We must reply with CLOSE, then teardown. */
        if (sshbuf_put_u8(out_reply, SSH_MSG_CHANNEL_CLOSE) != 0) return SSH_ERR_NO_MEM;
        if (sshbuf_put_u32(out_reply, c->remote_id) != 0) return SSH_ERR_NO_MEM;

        sshbuf_free(&c->in_data);
        memset(c, 0, sizeof(*c));
    }

    return 0;
}

static int handle_channel_request(ssh_channel_mgr_t *mgr, sshbuf_t *in, sshbuf_t *out_reply) {
    uint32_t recipient_channel = 0;
    if (sshbuf_get_u32(in, &recipient_channel) != 0) return SSH_ERR_INVALID_FORMAT;

    const uint8_t *rtype_ptr = NULL;
    size_t rtype_len = 0;
    if (sshbuf_get_string_ptr(in, &rtype_ptr, &rtype_len) != 0) return SSH_ERR_INVALID_FORMAT;

    uint8_t want_reply = 0;
    if (sshbuf_get_u8(in, &want_reply) != 0) return SSH_ERR_INVALID_FORMAT;

    ssh_channel_t *c = channel_lookup_local(mgr, recipient_channel);
    if (!c || c->state == SSH_CHANNEL_STATE_CLOSED) {
        if (want_reply) {
            if (sshbuf_put_u8(out_reply, SSH_MSG_CHANNEL_FAILURE) != 0) return SSH_ERR_NO_MEM;
            if (sshbuf_put_u32(out_reply, recipient_channel) != 0) return SSH_ERR_NO_MEM;
        }
        return 0;
    }

    bool accepted = false;

    if (rtype_len == 7 && memcmp(rtype_ptr, "pty-req", 7) == 0) {
        const uint8_t *term = NULL; size_t term_len = 0;
        uint32_t cols = 0, rows = 0, width_px = 0, height_px = 0;
        const uint8_t *modes = NULL; size_t modes_len = 0;

        if (sshbuf_get_string_ptr(in, &term, &term_len) == 0 &&
            sshbuf_get_u32(in, &cols) == 0 &&
            sshbuf_get_u32(in, &rows) == 0 &&
            sshbuf_get_u32(in, &width_px) == 0 &&
            sshbuf_get_u32(in, &height_px) == 0 &&
            sshbuf_get_string_ptr(in, &modes, &modes_len) == 0) {
            
            c->pty_requested = true;
            size_t t_copy = term_len < sizeof(c->pty_params.term) - 1 ? term_len : sizeof(c->pty_params.term) - 1;
            memcpy(c->pty_params.term, term, t_copy);
            c->pty_params.term[t_copy] = '\0';
            c->pty_params.cols = cols;
            c->pty_params.rows = rows;
            c->pty_params.width_px = width_px;
            c->pty_params.height_px = height_px;
            if (modes_len <= sizeof(c->pty_params.modes)) {
                memcpy(c->pty_params.modes, modes, modes_len);
                c->pty_params.modes_len = modes_len;
            }
            accepted = true;
        }
    } else if (rtype_len == 5 && memcmp(rtype_ptr, "shell", 5) == 0) {
        c->shell_requested = true;
        accepted = true;
    } else if (rtype_len == 4 && memcmp(rtype_ptr, "exec", 4) == 0) {
        const uint8_t *cmd = NULL; size_t cmd_len = 0;
        if (sshbuf_get_string_ptr(in, &cmd, &cmd_len) == 0) {
            c->exec_requested = true;
            size_t c_copy = cmd_len < sizeof(c->exec_cmd) - 1 ? cmd_len : sizeof(c->exec_cmd) - 1;
            memcpy(c->exec_cmd, cmd, c_copy);
            c->exec_cmd[c_copy] = '\0';
            accepted = true;
        }
    } else if (rtype_len == 13 && memcmp(rtype_ptr, "window-change", 13) == 0) {
        uint32_t cols = 0, rows = 0, width_px = 0, height_px = 0;
        if (sshbuf_get_u32(in, &cols) == 0 &&
            sshbuf_get_u32(in, &rows) == 0 &&
            sshbuf_get_u32(in, &width_px) == 0 &&
            sshbuf_get_u32(in, &height_px) == 0) {
            c->pty_params.cols = cols;
            c->pty_params.rows = rows;
            c->pty_params.width_px = width_px;
            c->pty_params.height_px = height_px;
            accepted = true;
        }
    } else if (rtype_len == 3 && memcmp(rtype_ptr, "env", 3) == 0) {
        const uint8_t *vname = NULL; size_t vname_len = 0;
        const uint8_t *vval = NULL; size_t vval_len = 0;
        if (sshbuf_get_string_ptr(in, &vname, &vname_len) == 0 &&
            sshbuf_get_string_ptr(in, &vval, &vval_len) == 0) {
            /* Admitted as no-op success */
            accepted = true;
        }
    } else {
        /* Unsupported request */
        accepted = false;
    }

    if (want_reply) {
        uint8_t resp = accepted ? SSH_MSG_CHANNEL_SUCCESS : SSH_MSG_CHANNEL_FAILURE;
        if (sshbuf_put_u8(out_reply, resp) != 0) return SSH_ERR_NO_MEM;
        if (sshbuf_put_u32(out_reply, c->remote_id) != 0) return SSH_ERR_NO_MEM;
    }

    return 0;
}

static int handle_global_request(sshbuf_t *in, sshbuf_t *out_reply) {
    const uint8_t *rname = NULL; size_t rname_len = 0;
    if (sshbuf_get_string_ptr(in, &rname, &rname_len) != 0) return SSH_ERR_INVALID_FORMAT;

    uint8_t want_reply = 0;
    if (sshbuf_get_u8(in, &want_reply) != 0) return SSH_ERR_INVALID_FORMAT;

    if (want_reply) {
        if (sshbuf_put_u8(out_reply, SSH_MSG_REQUEST_FAILURE) != 0) return SSH_ERR_NO_MEM;
    }
    return 0;
}

int channel_process_packet(ssh_channel_mgr_t *mgr, uint8_t type,
                           const uint8_t *payload, size_t len,
                           sshbuf_t *out_reply) {
    if (!mgr || !payload || len == 0 || !out_reply) return SSH_ERR_INVALID_ARG;

    sshbuf_t in;
    if (sshbuf_init_from(&in, payload, len) != 0) return SSH_ERR_INVALID_FORMAT;

    /* If payload begins with the message type byte, consume it */
    uint8_t peek = 0;
    if (sshbuf_peek_u8(&in, &peek) == 0 && peek == type) {
        sshbuf_get_u8(&in, &peek);
    }

    switch (type) {
        case SSH_MSG_CHANNEL_OPEN:
            return handle_channel_open(mgr, &in, out_reply);

        case SSH_MSG_CHANNEL_WINDOW_ADJUST:
            return handle_window_adjust(mgr, &in);

        case SSH_MSG_CHANNEL_DATA:
            return handle_channel_data(mgr, &in, out_reply);

        case SSH_MSG_CHANNEL_EXTENDED_DATA:
            return handle_channel_extended_data(mgr, &in, out_reply);

        case SSH_MSG_CHANNEL_EOF:
            return handle_channel_eof(mgr, &in);

        case SSH_MSG_CHANNEL_CLOSE:
            return handle_channel_close(mgr, &in, out_reply);

        case SSH_MSG_CHANNEL_REQUEST:
            return handle_channel_request(mgr, &in, out_reply);

        case SSH_MSG_GLOBAL_REQUEST:
            return handle_global_request(&in, out_reply);

        case SSH_MSG_CHANNEL_OPEN_CONFIRMATION:
        case SSH_MSG_CHANNEL_OPEN_FAILURE:
        case SSH_MSG_CHANNEL_SUCCESS:
        case SSH_MSG_CHANNEL_FAILURE:
            /* Ignored when received by server */
            return 0;

        default:
            return SSH_ERR_INVALID_ARG;
    }
}

int64_t channel_send_data(ssh_channel_mgr_t *mgr, uint32_t local_id,
                          const uint8_t *data, size_t len,
                          sshbuf_t *out_packet) {
    if (!mgr || (!data && len > 0) || !out_packet) return SSH_ERR_INVALID_ARG;

    ssh_channel_t *c = channel_lookup_local(mgr, local_id);
    if (!c || c->state == SSH_CHANNEL_STATE_CLOSED) return SSH_ERR_INVALID_ARG;

    if (c->state != SSH_CHANNEL_STATE_OPEN && c->state != SSH_CHANNEL_STATE_EOF_REMOTE) {
        return SSH_ERR_INVALID_FORMAT;
    }

    if (c->remote_window == 0) {
        return 0; /* Window exhausted */
    }

    size_t limit = c->remote_window < c->remote_maxpacket ? c->remote_window : c->remote_maxpacket;
    size_t to_send = len < limit ? len : limit;

    c->remote_window -= (uint32_t)to_send;

    if (sshbuf_put_u8(out_packet, SSH_MSG_CHANNEL_DATA) != 0) return SSH_ERR_NO_MEM;
    if (sshbuf_put_u32(out_packet, c->remote_id) != 0) return SSH_ERR_NO_MEM;
    if (sshbuf_put_string(out_packet, data, to_send) != 0) return SSH_ERR_NO_MEM;

    return (int64_t)to_send;
}

int64_t channel_send_stderr(ssh_channel_mgr_t *mgr, uint32_t local_id,
                            const uint8_t *data, size_t len,
                            sshbuf_t *out_packet) {
    if (!mgr || (!data && len > 0) || !out_packet) return SSH_ERR_INVALID_ARG;

    ssh_channel_t *c = channel_lookup_local(mgr, local_id);
    if (!c || c->state == SSH_CHANNEL_STATE_CLOSED) return SSH_ERR_INVALID_ARG;

    if (c->state != SSH_CHANNEL_STATE_OPEN && c->state != SSH_CHANNEL_STATE_EOF_REMOTE) {
        return SSH_ERR_INVALID_FORMAT;
    }

    if (c->remote_window == 0) {
        return 0; /* Window exhausted */
    }

    size_t limit = c->remote_window < c->remote_maxpacket ? c->remote_window : c->remote_maxpacket;
    size_t to_send = len < limit ? len : limit;

    c->remote_window -= (uint32_t)to_send;

    if (sshbuf_put_u8(out_packet, SSH_MSG_CHANNEL_EXTENDED_DATA) != 0) return SSH_ERR_NO_MEM;
    if (sshbuf_put_u32(out_packet, c->remote_id) != 0) return SSH_ERR_NO_MEM;
    if (sshbuf_put_u32(out_packet, SSH_EXTENDED_DATA_STDERR) != 0) return SSH_ERR_NO_MEM;
    if (sshbuf_put_string(out_packet, data, to_send) != 0) return SSH_ERR_NO_MEM;

    return (int64_t)to_send;
}

int channel_send_exit_status(ssh_channel_mgr_t *mgr, uint32_t local_id,
                             uint32_t status,
                             sshbuf_t *out_packet) {
    if (!mgr || !out_packet) return SSH_ERR_INVALID_ARG;

    ssh_channel_t *c = channel_lookup_local(mgr, local_id);
    if (!c || c->state == SSH_CHANNEL_STATE_CLOSED) return SSH_ERR_INVALID_ARG;

    c->exit_status_sent = true;
    c->exit_status = status;

    if (sshbuf_put_u8(out_packet, SSH_MSG_CHANNEL_REQUEST) != 0) return SSH_ERR_NO_MEM;
    if (sshbuf_put_u32(out_packet, c->remote_id) != 0) return SSH_ERR_NO_MEM;
    if (sshbuf_put_cstring(out_packet, "exit-status") != 0) return SSH_ERR_NO_MEM;
    if (sshbuf_put_u8(out_packet, 0) != 0) return SSH_ERR_NO_MEM; /* want_reply = FALSE */
    if (sshbuf_put_u32(out_packet, status) != 0) return SSH_ERR_NO_MEM;

    return 0;
}

int channel_send_eof(ssh_channel_mgr_t *mgr, uint32_t local_id,
                     sshbuf_t *out_packet) {
    if (!mgr || !out_packet) return SSH_ERR_INVALID_ARG;

    ssh_channel_t *c = channel_lookup_local(mgr, local_id);
    if (!c || c->state == SSH_CHANNEL_STATE_CLOSED) return SSH_ERR_INVALID_ARG;

    if (c->state == SSH_CHANNEL_STATE_OPEN) {
        c->state = SSH_CHANNEL_STATE_EOF_LOCAL;
    } else if (c->state == SSH_CHANNEL_STATE_EOF_REMOTE) {
        c->state = SSH_CHANNEL_STATE_EOF_BOTH;
    }

    if (sshbuf_put_u8(out_packet, SSH_MSG_CHANNEL_EOF) != 0) return SSH_ERR_NO_MEM;
    if (sshbuf_put_u32(out_packet, c->remote_id) != 0) return SSH_ERR_NO_MEM;

    return 0;
}

int channel_send_close(ssh_channel_mgr_t *mgr, uint32_t local_id,
                       sshbuf_t *out_packet) {
    if (!mgr || !out_packet) return SSH_ERR_INVALID_ARG;

    ssh_channel_t *c = channel_lookup_local(mgr, local_id);
    if (!c || c->state == SSH_CHANNEL_STATE_CLOSED) return SSH_ERR_INVALID_ARG;

    if (sshbuf_put_u8(out_packet, SSH_MSG_CHANNEL_CLOSE) != 0) return SSH_ERR_NO_MEM;
    if (sshbuf_put_u32(out_packet, c->remote_id) != 0) return SSH_ERR_NO_MEM;

    if (c->state == SSH_CHANNEL_STATE_CLOSE_RCVD) {
        /* Both sides closed */
        sshbuf_free(&c->in_data);
        memset(c, 0, sizeof(*c));
    } else {
        c->state = SSH_CHANNEL_STATE_CLOSE_SENT;
    }

    return 0;
}

size_t channel_read_inbound(ssh_channel_mgr_t *mgr, uint32_t local_id,
                            uint8_t *buf, size_t max_len) {
    if (!mgr || !buf || max_len == 0) return 0;

    ssh_channel_t *c = channel_lookup_local(mgr, local_id);
    if (!c) return 0;

    size_t avail = sshbuf_len(&c->in_data);
    if (avail == 0) return 0;

    size_t to_read = avail < max_len ? avail : max_len;
    memcpy(buf, sshbuf_ptr(&c->in_data), to_read);
    sshbuf_consume(&c->in_data, to_read);

    return to_read;
}

uint32_t channel_get_write_avail(ssh_channel_mgr_t *mgr, uint32_t local_id) {
    if (!mgr) return 0;
    ssh_channel_t *c = channel_lookup_local(mgr, local_id);
    if (!c) return 0;
    if (c->state != SSH_CHANNEL_STATE_OPEN && c->state != SSH_CHANNEL_STATE_EOF_REMOTE) {
        return 0;
    }
    return c->remote_window;
}
