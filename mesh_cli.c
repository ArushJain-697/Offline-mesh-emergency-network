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
      /* \r returns to start of line, \033[K erases to end of line.
         This clears whatever the user was typing before printing. */
      printf("\r\033[K[RECEIVED] %s\nChoose: ", buffer);
      fflush(stdout);
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
  /* Usage:
       ./mesh_cli <port> <password>                        — auto-discovery
       ./mesh_cli <port> <password> <helper_ip> <helper_port> — targeted bootstrap */
  if (argc < 3) {
    fprintf(stderr, "Usage: %s <port> <password> [helper_ip helper_port]\n", argv[0]);
    return 1;
  }
  
  int my_port = atoi(argv[1]);
  if (my_port <= 1024 || my_port > 65535) {
    fprintf(stderr, "Port must be between 1025 and 65535\n");
    return 1;
  }

  const char *password    = argv[2];
  const char *helper_ip   = (argc >= 5) ? argv[3] : NULL;
  int         helper_port = (argc >= 5) ? atoi(argv[4]) : 0;

  backend_bootstrap(my_port, password, helper_ip, helper_port);

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
      backend_leave();
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