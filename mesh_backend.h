#ifndef MESH_BACKEND_H
#define MESH_BACKEND_H

void backend_init(char name);
void backend_send_message(char to, const char *msg);
void backend_broadcast(const char *msg);
int backend_receive(char *out, int max_len);
void backend_leave(void);
void backend_close(void);

#endif

