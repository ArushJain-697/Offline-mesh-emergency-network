// mesh_cli.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include "mesh_backend.h"

static char local_node = '?';
static int running = 1;

// Listens for incoming chat messages (updates happen in background)
void *receiver_thread_function(void *arg) {
    (void)arg;
    char buffer[700];

    while (running) {
        // Blocking call - waits for message
        int bytes = backend_receive(buffer, sizeof(buffer));
        if (bytes > 0) {
            printf("\n\n");
            printf(">>> %s\n", buffer);
            printf("\n(%c) Enter choice: ", local_node);
            fflush(stdout);
        }
    }
    return NULL;
}

int read_line(char *buffer, int size) {
    if (!fgets(buffer, size, stdin)) return 0;
    size_t len = strlen(buffer);
    if (len > 0 && buffer[len - 1] == '\n') buffer[len - 1] = '\0';
    return 1;
}

void print_menu(void) {
    printf("\n");
    printf("+============= MESH CHAT (%c) ==============+\n", local_node);
    printf("|  1. Send Direct Message                  |\n");
    printf("|  2. Broadcast (Send to All)              |\n");
    printf("|  3. Exit                                 |\n");
    printf("|  4. Refresh Connection (Sync Nodes)      |\n");
    printf("+==========================================+\n");
    printf("Choice: ");
    fflush(stdout);
}

int main(void) {
    pthread_t rthread;
    char inbuf[700], msg[600];
    int choice;
    char to_char;

    printf("Starting Mesh Chat...\n");

    // 1. Initialize Backend (Detect IP + Auto Join)
    if (backend_init_auto() != 0) {
        printf("Failed to initialize backend.\n");
        return 1;
    }
    
    local_node = backend_get_local_node();
    
    // 2. Start Receiver Thread
    pthread_create(&rthread, NULL, receiver_thread_function, NULL);

    while (running) {
        print_menu();

        if (!read_line(inbuf, sizeof(inbuf))) break;

        if (sscanf(inbuf, "%d", &choice) != 1) continue;

        if (choice == 1) {
            printf("Target Node (A-E): ");
            if (read_line(inbuf, sizeof(inbuf))) {
                sscanf(inbuf, " %c", &to_char);
                printf("Message: ");
                if (read_line(msg, sizeof(msg))) {
                    backend_send_message(toupper(to_char), msg);
                    printf("[sent]\n");
                }
            }
        }
        else if (choice == 2) {
            printf("Broadcast Message: ");
            if (read_line(msg, sizeof(msg))) {
                backend_broadcast(msg);
                printf("[broadcast sent]\n");
            }
        }
        else if (choice == 3) {
            running = 0;
            break;
        }
        else if (choice == 4) {
            printf("Broadcasting presence to network...\n");
            backend_force_sync();
            printf("Done. Wait 2 seconds for responses.\n");
        }
        else {
            printf("Invalid choice.\n");
        }
    }

    backend_close();
    return 0;
}
