#ifndef FORTRESS_SSH_PACKET_H
#define FORTRESS_SSH_PACKET_H

#define SSH_VERSION_STRING "SSH-2.0-FortressOS_uSSH"

/* Transport layer message numbers (RFC 4250, RFC 4253) */
#define SSH_MSG_DISCONNECT                1
#define SSH_MSG_IGNORE                    2
#define SSH_MSG_UNIMPLEMENTED             3
#define SSH_MSG_DEBUG                     4
#define SSH_MSG_SERVICE_REQUEST           5
#define SSH_MSG_SERVICE_ACCEPT            6
#define SSH_MSG_KEXINIT                   20
#define SSH_MSG_NEWKEYS                   21

/* Key exchange specific message numbers (RFC 8731) */
#define SSH_MSG_KEX_ECDH_INIT             30
#define SSH_MSG_KEX_ECDH_REPLY            31

/* User authentication message numbers (RFC 4252) */
#define SSH_MSG_USERAUTH_REQUEST          50
#define SSH_MSG_USERAUTH_FAILURE          51
#define SSH_MSG_USERAUTH_SUCCESS          52
#define SSH_MSG_USERAUTH_BANNER           53
#define SSH_MSG_USERAUTH_PK_OK            60

/* Connection protocol message numbers (RFC 4254) */
#define SSH_MSG_GLOBAL_REQUEST            80
#define SSH_MSG_REQUEST_SUCCESS           81
#define SSH_MSG_REQUEST_FAILURE           82
#define SSH_MSG_CHANNEL_OPEN              90
#define SSH_MSG_CHANNEL_OPEN_CONFIRMATION 91
#define SSH_MSG_CHANNEL_OPEN_FAILURE      92
#define SSH_MSG_CHANNEL_WINDOW_ADJUST     93
#define SSH_MSG_CHANNEL_DATA              94
#define SSH_MSG_CHANNEL_EXTENDED_DATA     95
#define SSH_MSG_CHANNEL_EOF               96
#define SSH_MSG_CHANNEL_CLOSE             97
#define SSH_MSG_CHANNEL_REQUEST           98
#define SSH_MSG_CHANNEL_SUCCESS           99
#define SSH_MSG_CHANNEL_FAILURE           100

/* Disconnect reason codes (RFC 4250) */
#define SSH_DISCONNECT_HOST_NOT_ALLOWED_TO_CONNECT      1
#define SSH_DISCONNECT_PROTOCOL_ERROR                   2
#define SSH_DISCONNECT_KEY_EXCHANGE_FAILED              3
#define SSH_DISCONNECT_RESERVED                         4
#define SSH_DISCONNECT_MAC_ERROR                        5
#define SSH_DISCONNECT_COMPRESSION_ERROR                6
#define SSH_DISCONNECT_SERVICE_NOT_AVAILABLE            7
#define SSH_DISCONNECT_PROTOCOL_VERSION_NOT_SUPPORTED   8
#define SSH_DISCONNECT_HOST_KEY_NOT_VERIFIABLE          9
#define SSH_DISCONNECT_CONNECTION_LOST                  10
#define SSH_DISCONNECT_BY_APPLICATION                   11
#define SSH_DISCONNECT_TOO_MANY_CONNECTIONS             12
#define SSH_DISCONNECT_AUTH_CANCELLED_BY_USER           13
#define SSH_DISCONNECT_NO_MORE_AUTH_METHODS_AVAILABLE   14
#define SSH_DISCONNECT_ILLEGAL_USER_NAME                15

/* Framing bounds */
#define SSH_MIN_PACKET_LEN   5
#define SSH_MAX_PACKET_LEN   35000
#define SSH_POLY1305_TAG_LEN 16

/* Algorithm names */
#define SSH_KEX_CURVE25519_SHA256     "curve25519-sha256"
#define SSH_HOSTKEY_ED25519           "ssh-ed25519"
#define SSH_CIPHER_CHACHA20_POLY1305  "chacha20-poly1305@openssh.com"
#define SSH_KEX_STRICT_SERVER         "kex-strict-s-v00@openssh.com"
#define SSH_KEX_STRICT_CLIENT         "kex-strict-c-v00@openssh.com"

#endif /* FORTRESS_SSH_PACKET_H */
