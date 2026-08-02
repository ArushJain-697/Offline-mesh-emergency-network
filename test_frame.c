/* Self-check for the binary framing layer. Build & run: make test */
#include "mesh_frame.h"
#include <sodium.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    assert(sodium_init() >= 0);

    /* round-trip: encode with a fixed id, decode gets payload + header back */
    uint8_t id[MESH_MSGID_BYTES] = {1,2,3,4,5,6,7,8};
    const char *msg = "DIR:0:A:hello flood zone";
    uint8_t out[256];
    int fl = mesh_frame_encode(3, id, msg, strlen(msg), out, sizeof(out));
    assert(fl == MESH_HDR_BYTES + (int)strlen(msg));

    struct mesh_hdr hdr;
    const char *payload;
    int pl = mesh_frame_decode(out, fl, &hdr, &payload);
    assert(pl == (int)strlen(msg));
    assert(memcmp(payload, msg, pl) == 0);
    assert(hdr.version == MESH_PROTO_VERSION);
    assert(hdr.ttl == 3);
    assert(memcmp(hdr.msg_id, id, MESH_MSGID_BYTES) == 0);

    /* wrong version is rejected */
    out[0] = 99;
    assert(mesh_frame_decode(out, fl, &hdr, &payload) == -1);

    /* runt shorter than the header is rejected */
    assert(mesh_frame_decode(out, MESH_HDR_BYTES - 1, &hdr, &payload) == -1);

    /* null id path produces distinct random ids */
    uint8_t a[64], b[64];
    mesh_frame_encode(7, NULL, "x", 1, a, sizeof(a));
    mesh_frame_encode(7, NULL, "x", 1, b, sizeof(b));
    assert(memcmp(a + 4, b + 4, MESH_MSGID_BYTES) != 0);

    printf("test_frame: all assertions passed\n");
    return 0;
}
