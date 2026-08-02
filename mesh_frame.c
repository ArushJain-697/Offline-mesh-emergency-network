#include "mesh_frame.h"
#include <sodium.h>
#include <string.h>

int mesh_frame_encode(uint8_t ttl, const uint8_t *id,
                      uint8_t frag_index, uint8_t frag_count,
                      const char *payload, int payload_len,
                      uint8_t *out, int out_len)
{
    if (payload_len < 0 || out_len < MESH_HDR_BYTES + payload_len) return -1;

    out[0] = MESH_PROTO_VERSION;
    out[1] = ttl;
    out[2] = frag_index;
    out[3] = frag_count;
    if (id) memcpy(out + 4, id, MESH_MSGID_BYTES);
    else    randombytes_buf(out + 4, MESH_MSGID_BYTES);

    if (payload_len > 0) memcpy(out + MESH_HDR_BYTES, payload, payload_len);
    return MESH_HDR_BYTES + payload_len;
}

int mesh_frame_decode(const uint8_t *buf, int buf_len,
                      struct mesh_hdr *hdr, const char **payload)
{
    if (buf_len < MESH_HDR_BYTES)      return -1;
    if (buf[0] != MESH_PROTO_VERSION)  return -1;

    if (hdr) {
        hdr->version    = buf[0];
        hdr->ttl        = buf[1];
        hdr->frag_index = buf[2];
        hdr->frag_count = buf[3];
        memcpy(hdr->msg_id, buf + 4, MESH_MSGID_BYTES);
    }
    if (payload) *payload = (const char *)(buf + MESH_HDR_BYTES);
    return buf_len - MESH_HDR_BYTES;
}
