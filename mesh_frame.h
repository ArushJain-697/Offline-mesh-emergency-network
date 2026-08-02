#ifndef MESH_FRAME_H
#define MESH_FRAME_H

#include <stdint.h>
#include <stddef.h>

/* Compact binary header prepended to every mesh packet (before encryption).
 * Fixed 12 bytes so an MCU can parse it without sscanf and so a LoRa frame
 * spends as little of its ~250-byte MTU on framing as possible.
 *
 *   [0]      version   protocol version (reject mismatches during rollout)
 *   [1]      ttl       hops remaining (multi-hop relay decrements this)
 *   [2]      flags     reserved: fragmentation etc. (0 for now)
 *   [3]      reserved  padding / future use
 *   [4..11]  msg_id    random id for dedup + replay drop
 */

#define MESH_PROTO_VERSION 1
#define MESH_MSGID_BYTES   8
#define MESH_HDR_BYTES     (4 + MESH_MSGID_BYTES)   /* = 12 */
#define MESH_DEFAULT_TTL   7                         /* matches bitchat's hop cap */

struct mesh_hdr {
    uint8_t version;
    uint8_t ttl;
    uint8_t flags;
    uint8_t reserved;
    uint8_t msg_id[MESH_MSGID_BYTES];
};

/* Build header+payload into out. If id is NULL a fresh random id is generated
 * (new-origin message); pass an existing id to preserve it (relay).
 * Returns total framed length, or -1 if out is too small. */
int mesh_frame_encode(uint8_t ttl, const uint8_t *id,
                      const char *payload, int payload_len,
                      uint8_t *out, int out_len);

/* Parse a framed packet. Fills hdr (if non-NULL) and points *payload into buf.
 * Returns payload length, or -1 if malformed or wrong protocol version. */
int mesh_frame_decode(const uint8_t *buf, int buf_len,
                      struct mesh_hdr *hdr, const char **payload);

#endif /* MESH_FRAME_H */
