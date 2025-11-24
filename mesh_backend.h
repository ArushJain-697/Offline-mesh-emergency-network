// mesh_backend.h
// Updated header for auto-IP mesh backend

#ifndef MESH_BACKEND_H
#define MESH_BACKEND_H

#include <stdint.h>

// Maximum nodes in our mesh
#define MAX_NODES 5

// ---------- Initialization ----------
// Automatically detects local IP, assigns node A–E,
// updates nodes.dat, syncs file to other nodes.
int backend_init_auto(void);

// ---------- Messaging API (unchanged) ----------

// Receive a user-level message (blocking).
// Returns number of bytes copied to 'out',
// or 0 if the received packet is not a user message (e.g., nodes update).
int backend_receive(char *out, int max_len);

// Send direct message to a specific node (A–E)
void backend_send_message(char to, const char *msg);

// Broadcast message to all nodes except yourself
void backend_broadcast(const char *msg);

// Graceful shutdown: stops threads, resets our IP slot in nodes.dat,
// broadcasts updated nodes.dat to other nodes.
void backend_close(void);

#endif
