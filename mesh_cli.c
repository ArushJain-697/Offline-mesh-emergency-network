// mesh_cli.c
// Main chat CLI using the auto-IP backend (backend_init_auto)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "mesh_backend.h"

static char local_node = '?';
static int running = 1;

void *receiver_thread_function(void *arg) {
    (void)arg;
    char buffer[700];

    while (running) {
        int bytes = backend_receive(buffer, sizeof(buffer));
        if (bytes > 0) {
            printf("\n");
            printf("+---------- MESSAGE RECEIVED ----------+\n");
            printf("| %s\n", buffer);
            printf("+--------------------------------------+\n");
            printf("(%c) Enter choice: ", local_node);
            fflush(stdout);
        }
    }
    return NULL;
}

int read_line(char *buffer, int size) {
    char *res = fgets(buffer, size, stdin);
    if (!res) return 0;

    int len = strlen(buffer);
    if (len > 0 && buffer[len - 1] == '\n')
        buffer[len - 1] = '\0';

    return 1;
}

void print_menu(void) {
    printf("\n");
    printf("+============= RUDP MESH CHAT =============+\n");
    printf("|  Node: %c                                 |\n", local_node);
    printf("|  1. Send direct message                  |\n");
    printf("|  2. Broadcast to all nodes               |\n");
    printf("|  3. Quit                                 |\n");
    printf("+==========================================+\n");
    printf("(%c) Enter choice: ", local_node);
    fflush(stdout);
}

int main(void) {
    pthread_t rthread;
    char inbuf[700], msg[600];
    int choice;
    char to_char;

    printf("\n");
    printf("+==========================================+\n");
    printf("|     RELIABLE UDP MESH CHAT SYSTEM        |\n");
    printf("|     Auto-IP + Distributed nodes.dat      |\n");
    printf("+==========================================+\n\n");

    // initialize backend automatically
    if (backend_init_auto() != 0) {
        printf("Backend init failed.\n");
        return 1;
    }

    // After init, backend printed node; retrieve from file
    FILE *fp = fopen("nodes.dat", "r");
    if (fp) {
        char n; char ip[64];
        while (fscanf(fp, " %c %63s", &n, ip) == 2) {
            if (strcmp(ip, "0.0.0.0") != 0) {
                // guess local node by matching our IP again
                // backend already printed correct node so let's trust it:
                // find first non-zero entry that matches local node
            }
        }
        fclose(fp);
    }

    // Ask backend what local node is (simple trick: backend prints node)
    // OR store it globally; easiest solution:
    extern char local_node_external;
    local_node = local_node_external;

    // Start receiver thread
    pthread_create(&rthread, NULL, receiver_thread_function, NULL);

    while (running) {
        print_menu();

        if (!read_line(inbuf, sizeof(inbuf)))
            break;

        if (sscanf(inbuf, "%d", &choice) != 1) {
            printf("Please enter a number.\n");
            continue;
        }

        if (choice == 1) {
            printf("Enter destination node (A-E): ");
            if (!read_line(inbuf, sizeof(inbuf))) continue;
            sscanf(inbuf, " %c", &to_char);

            printf("Enter message: ");
            if (!read_line(msg, sizeof(msg))) continue;

            backend_send_message(toupper(to_char), msg);
            printf("[SENT] Direct message sent.\n");
        }
        else if (choice == 2) {
            printf("Enter broadcast message: ");
            if (!read_line(msg, sizeof(msg))) continue;

            backend_broadcast(msg);
            printf("[SENT] Broadcast sent.\n");
        }
        else if (choice == 3) {
            running = 0;
            break;
        }
        else {
            printf("Invalid choice.\n");
        }
    }

    backend_close();
    return 0;
}
