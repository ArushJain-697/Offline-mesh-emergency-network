// mesh_backend.h
#ifndef MESH_BACKEND_H
#define MESH_BACKEND_H

#include <stdint.h>

#define MAX_NODES 5

// Automatic initialization: Detects IP, assigns Node, Broadcasts existence
int backend_init_auto(void);
char backend_get_local_node(void);

// Messaging
int backend_receive(char *out, int max_len);
void backend_send_message(char to, const char *msg);
void backend_broadcast(const char *msg);

// NEW: Allows the user to manually trigger a "Join" broadcast again
// Useful if the other person came online AFTER you started.
void backend_force_sync(void);

void backend_close(void);

#endif
