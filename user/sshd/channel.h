#ifndef FORTRESS_SSH_CHANNEL_H
#define FORTRESS_SSH_CHANNEL_H

#include "types.h"
#include "sshbuf.h"
#include "ssh_packet.h"

#define SSH_CHANNEL_MAX_CHANNELS       4
#define SSH_CHANNEL_DEFAULT_WINDOW     (64 * 1024)
#define SSH_CHANNEL_DEFAULT_MAXPACKET   32768
#define SSH_CHANNEL_MIN_WINDOW_ADJUST   16384

/* RFC 4254 Open Failure Reason Codes */
#define SSH_OPEN_ADMINISTRATIVELY_PROHIBITED 1
#define SSH_OPEN_CONNECT_FAILED              2
#define SSH_OPEN_UNKNOWN_CHANNEL_TYPE        3
#define SSH_OPEN_RESOURCE_SHORTAGE           4

/* RFC 4254 Extended Data Type Codes */
#define SSH_EXTENDED_DATA_STDERR             1

typedef enum {
    SSH_CHANNEL_STATE_CLOSED = 0,
    SSH_CHANNEL_STATE_OPEN,
    SSH_CHANNEL_STATE_EOF_LOCAL,      /* We sent EOF */
    SSH_CHANNEL_STATE_EOF_REMOTE,     /* Remote sent EOF */
    SSH_CHANNEL_STATE_EOF_BOTH,       /* Both sent EOF */
    SSH_CHANNEL_STATE_CLOSE_SENT,     /* We sent CLOSE, awaiting remote CLOSE */
    SSH_CHANNEL_STATE_CLOSE_RCVD      /* Remote sent CLOSE, awaiting local CLOSE */
} ssh_channel_state_t;

typedef struct {
    char term[64];
    uint32_t cols;
    uint32_t rows;
    uint32_t width_px;
    uint32_t height_px;
    uint8_t modes[256];
    size_t modes_len;
} ssh_pty_params_t;

typedef struct {
    uint32_t local_id;
    uint32_t remote_id;
    ssh_channel_state_t state;
    
    /* Flow control */
    uint32_t local_window;
    uint32_t local_maxpacket;
    uint32_t remote_window;
    uint32_t remote_maxpacket;

    /* PTY and session details */
    bool pty_requested;
    ssh_pty_params_t pty_params;
    bool shell_requested;
    bool exec_requested;
    char exec_cmd[512];
    
    /* Inbound data received from client */
    sshbuf_t in_data;
    
    /* Exit status sent */
    bool exit_status_sent;
    uint32_t exit_status;
} ssh_channel_t;

typedef struct {
    ssh_channel_t channels[SSH_CHANNEL_MAX_CHANNELS];
    uint32_t next_local_id;
} ssh_channel_mgr_t;

void channel_mgr_init(ssh_channel_mgr_t *mgr);
void channel_mgr_cleanup(ssh_channel_mgr_t *mgr);

/* Lookup channel by local or remote ID */
ssh_channel_t *channel_lookup_local(ssh_channel_mgr_t *mgr, uint32_t local_id);
ssh_channel_t *channel_lookup_remote(ssh_channel_mgr_t *mgr, uint32_t remote_id);

/* Process connection protocol messages (SSH_MSG_CHANNEL_* and SSH_MSG_GLOBAL_REQUEST).
 * Returns:
 *   0 on success (out_reply may contain 0 or more response packets)
 *   negative on protocol error or fatal condition
 */
int channel_process_packet(ssh_channel_mgr_t *mgr, uint8_t type,
                           const uint8_t *payload, size_t len,
                           sshbuf_t *out_reply);

/* Server application / bridge actions: */

/* Send data on a channel to remote client. Returns number of bytes accepted (> 0),
 * 0 if remote window is full, or negative error code. */
int64_t channel_send_data(ssh_channel_mgr_t *mgr, uint32_t local_id,
                          const uint8_t *data, size_t len,
                          sshbuf_t *out_packet);

/* Send stderr data on a channel (extended data type 1) */
int64_t channel_send_stderr(ssh_channel_mgr_t *mgr, uint32_t local_id,
                            const uint8_t *data, size_t len,
                            sshbuf_t *out_packet);

/* Send exit-status request to client (RFC 4254 §6.10) */
int channel_send_exit_status(ssh_channel_mgr_t *mgr, uint32_t local_id,
                             uint32_t status,
                             sshbuf_t *out_packet);

/* Send EOF on channel */
int channel_send_eof(ssh_channel_mgr_t *mgr, uint32_t local_id,
                     sshbuf_t *out_packet);

/* Send CLOSE on channel */
int channel_send_close(ssh_channel_mgr_t *mgr, uint32_t local_id,
                       sshbuf_t *out_packet);

/* Consume inbound data from client received on channel */
size_t channel_read_inbound(ssh_channel_mgr_t *mgr, uint32_t local_id,
                            uint8_t *buf, size_t max_len);

/* Check if channel can accept outbound data (returns available remote window) */
uint32_t channel_get_write_avail(ssh_channel_mgr_t *mgr, uint32_t local_id);

#endif /* FORTRESS_SSH_CHANNEL_H */
