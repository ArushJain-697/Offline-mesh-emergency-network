#ifndef MESH_DISCOVERY_H
#define MESH_DISCOVERY_H

#include <stddef.h>

#define DISCOVERY_PORT 9000

/* Open the shared discovery socket on port 9000.
   Returns fd on success, -1 on failure. */
int  discovery_init(void);

/* Return the discovery fd (for use in select()). */
int  discovery_get_fd(void);

/* Close the discovery socket. */
void discovery_close(void);

/* Discover our own LAN IP by asking the OS which interface
   it would route through. Writes result into out[len]. */
void discovery_get_my_ip(char *out, size_t len);

/* 3-layer bootstrap.
   Sends NET_DISCOVER on subnet broadcast, LAN broadcast, then
   targeted helper (if helper_ip != NULL). Listens on sock_fd
   (already bound to my_port) for a NET_WELCOME response.
   Returns a malloc'd NET_WELCOME string on success (caller frees).
   Returns NULL → nobody answered, caller should become genesis node. */
char *discovery_bootstrap(int sock_fd, int my_port,
                           const char *helper_ip, int helper_port);

/* Called by the receiver thread when select() reports data on disc_fd.
   Reads the NET_DISCOVER packet and calls on_new_peer(ip, port) so
   mesh_backend can assign a letter and send NET_WELCOME. */
typedef void (*on_new_peer_fn)(const char *ip, int port);
void discovery_handle_incoming(on_new_peer_fn on_new_peer);

#endif /* MESH_DISCOVERY_H */
