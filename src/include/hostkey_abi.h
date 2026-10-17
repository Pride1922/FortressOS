#ifndef FORTRESS_HOSTKEY_ABI_H
#define FORTRESS_HOSTKEY_ABI_H
#include "types.h"
/* Root + CAP_SYS_ADMIN only. No arbitrary paths or mounts. READ is a private
 * monitor/provisioner load, never an unprivileged status/export operation.
 * PUBLISH consumes an already validated bounded OpenSSH container; kernel
 * storage deliberately does not implement or trust crypto. Every consumer
 * must validate the returned container before using the key. No ready flag.
 * (op, buffer, bytes): READ requires capacity 1024, returns length; PUBLISH
 * requires 1..1023 input bytes, returns zero. No output on failure. */
#define HOSTKEY_READ 0u
#define HOSTKEY_PUBLISH 1u
#define HOSTKEY_FILE_MAX 1024u
#define HOSTKEY_NAME "ssh_host_ed25519_key"
#define HOSTKEY_STAGE ".fortress-hostkey.pending"
#endif
