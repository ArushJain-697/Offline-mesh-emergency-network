#include "mesh_backend.h"
#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>   /* required for _Atomic (C11) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static _Atomic int running = 1;
void *receiver_thread(void *x) {
  char buffer[1024];
  while (running) {
    int n = backend_receive(buffer, sizeof(buffer));
    if (n > 0) {
      printf("\n[RECEIVED] %s\n", buffer);
      printf("Enter option: ");
      fflush(stdout);
    } else {
      usleep(100 * 1000);
    }
  }
  return NULL;
}
static int get_line(char *out, size_t len) {
  if (fgets(out, (int)len, stdin) == NULL)
    return 0;
  size_t l = strlen(out);
  if (l > 0 && out[l - 1] == '\n')
    out[l - 1] = '\0';
  return 1;
}
int main(int argc, char **argv) {
  char me;
  if (argc >= 2) {
    /* Letter passed as argument — assigned by add_node, guaranteed no collision. */
    me = toupper((unsigned char)argv[1][0]);
    printf("Starting as node %c (assigned by add_node)\n", me);
  } else {
    /* Fallback: prompt — use only if add_node told you this letter. */
    char me_char[8];
    printf("Enter node letter (must be the one assigned by add_node): ");
    if (!get_line(me_char, sizeof(me_char)))
      return 0;
    me = toupper((unsigned char)me_char[0]);
  }
  backend_init(me);

  pthread_t t;
  if (pthread_create(&t, NULL, receiver_thread, NULL) != 0) {
    perror("pthread_create");
    return 1;
  }
  char line[1024];   /* input buffer for menu choices */
  while (1) {
    printf("\n1) Send message\n");
    printf("2) Broadcast\n");
    printf("3) Exit\n");
    printf("Choose: ");
    if (!get_line(line, sizeof(line))) {
      printf("Input error, exiting.\n");
      break;
    }
    int op = atoi(line);
    if (op == 1) {
      char dest_line[8];
      char msg[512];
      printf("Send to node: ");
      if (!get_line(dest_line, sizeof(dest_line)))
        continue;
      char to = toupper((unsigned char)dest_line[0]);
      printf("Message: ");
      if (!get_line(msg, sizeof(msg)))
        continue;
      backend_send_message(to, msg);
    } else if (op == 2) {
      char msg[512];
      printf("Broadcast message: ");
      if (!get_line(msg, sizeof(msg)))
        continue;
      backend_broadcast(msg);
    } else if (op == 3) {
      running = 0;
      break;
    } else {
      printf("Invalid.\n");
    }
  }
  /* Bug fix #5: join the receiver thread FIRST so it finishes its last
     recvfrom() call before we close the socket underneath it. */
  pthread_join(t, NULL);
  backend_close();
  return 0;
}
