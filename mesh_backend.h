#ifndef MESH_BACKEND_H
#define MESH_BACKEND_H

// initialize UDP socket
void backend_init(char name);

// send direct message (A ➜ B)
void backend_send_message(char to, const char *msg);

// broadcast to all nodes
void backend_broadcast(const char *msg);

// receiver thread function (pass callback)
void *backend_receiver_thread(void *callback);

// close node cleanly
void backend_close(void);

#endif
