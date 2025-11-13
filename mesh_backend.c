// -----------------------------------------------------------
// mesh_backend.c
// -----------------------------------------------------------
// Backend for Open Mesh Emergency Network
// Handles UDP send/receive, timestamps, logging, broadcast, etc.
// -----------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <time.h>
#include <pthread.h>
#include "mesh_backend.h"

// -----------------------------------------------------------
// GLOBAL VARIABLES
// -----------------------------------------------------------
static int sock;
static char node_name;
static struct sockaddr_in addr, dest;
static int sent_count = 0, recv_count = 0;

// -----------------------------------------------------------
// Helper Functions
// -----------------------------------------------------------
// Node configuration table
struct NodeInfo {
    char name;
    int port;
    char ip[32];
};

// ✨ Replace these IPs with the real ones of each PC
struct NodeInfo nodes[] = {
    {'A', 9001, "10.138.204.116"},  // Your MacBook (Node A)
    {'B', 9002, "10.138.204.180"},  // Friend’s laptop
    {'C', 9003, "0.0.0.0"},  // Another PC
    {'D', 9004, "0.0.0.0"},  // Fourth PC
    {'E', 9005, "0.0.0.0"}   // Fifth PC
};

// Get port number for a node
int get_port(char node) {
    for (int i = 0; i < 5; i++) {
        if (nodes[i].name == node)
            return nodes[i].port;
    }
    return -1;
}

// Get IP address for a node
const char* get_ip(char node) {
    for (int i = 0; i < 5; i++) {
        if (nodes[i].name == node)
            return nodes[i].ip;
    }
    return "127.0.0.1"; // fallback (loopback)
}


void get_time_str(char *out) {
    time_t t;
    struct tm *tmp;
    t = time(NULL);
    tmp = localtime(&t);
    sprintf(out, "%02d:%02d:%02d", tmp->tm_hour, tmp->tm_min, tmp->tm_sec);
}

void log_message(const char *text) {
    char fname[30];
    sprintf(fname, "chat_log_%c.txt", node_name);
    FILE *f = fopen(fname, "a");
    if (f == NULL) return;
    fprintf(f, "%s\n", text);
    fclose(f);
}

const char* get_color(char node) {
    switch(node) {
        case 'A': return "\033[1;31m"; // red
        case 'B': return "\033[1;32m"; // green
        case 'C': return "\033[1;33m"; // yellow
        case 'D': return "\033[1;34m"; // blue
        case 'E': return "\033[1;35m"; // magenta
        default:  return "\033[0m";   // reset
    }
}

// -----------------------------------------------------------
// FUNCTION: backend_init
// -----------------------------------------------------------
void backend_init(char name) {
    node_name = name;
    int my_port = get_port(node_name);

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        exit(1);
    }

    addr.sin_family = AF_INET;
    addr.sin_port = htons(my_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }

    printf("\n\033[1;36m==========================================\n");
    printf(" NODE %c BACKEND STARTED (Port %d)\n", node_name, my_port);
    printf("==========================================\033[0m\n\n");
}

// -----------------------------------------------------------
// FUNCTION: backend_send_message
// -----------------------------------------------------------
void backend_send_message(char to, const char *msg) {
    char packet[700];
    char tstamp[20];

    int port = get_port(to);
    if (port == -1) return;

    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    dest.sin_addr.s_addr = inet_addr(get_ip(to));


    get_time_str(tstamp);
    sprintf(packet, "[%c ➜ %c @%s]: %s",
        node_name, to, tstamp, msg);

    sendto(sock, packet, strlen(packet), 0,
           (struct sockaddr*)&dest, sizeof(dest));

    sent_count++;
    log_message(packet);
}

// -----------------------------------------------------------
// FUNCTION: backend_broadcast
// -----------------------------------------------------------
void backend_broadcast(const char *msg) {
    char packet[700];
    char tstamp[20];

    get_time_str(tstamp);
    sprintf(packet, "[%c ➜ ALL @%s]: %s",
            node_name, tstamp, msg);

    for (char n = 'A'; n <= 'E'; n++) {
        if (n == node_name) continue; 

        int port = get_port(n);
        if (port == -1) continue;

        dest.sin_family = AF_INET;
        dest.sin_port = htons(port);
        dest.sin_addr.s_addr = inet_addr(get_ip(n));


        sendto(sock, packet, strlen(packet), 0,
               (struct sockaddr*)&dest, sizeof(dest));
    }

    sent_count++;
    log_message(packet);
}

// -----------------------------------------------------------
// FUNCTION: backend_receiver_thread
// ✅ --- THIS IS THE FIX --- ✅
// -----------------------------------------------------------
void *backend_receiver_thread(void *callback) {
    void (*append_callback)(const char *msg);
    append_callback = (void (*)(const char *))callback;

    char buf[700];
    while (1) {
        int n = recvfrom(sock, buf, sizeof(buf)-1, 0, NULL, NULL);
        if (n > 0) {
            buf[n] = '\0';
            recv_count++;

            // ✅ --- NEW FIX --- ✅
            // Check if the message is from ourselves.
            // The message format is "[A ➜ ..."
            // We check the 2nd char (index 1)
            if (buf[0] == '[' && buf[1] == node_name) {
                // This is a loopback. Ignore it.
                // This message will print to your TERMINAL, not the GUI
                fprintf(stderr, "--- BACKEND: Ignored loopback: %s ---\n", buf);
                fflush(stderr);
                continue; 
            }
            // ✅ --- END FIX --- ✅

            append_callback(buf); // display in GUI or console
            log_message(buf);
        }
    }
    return NULL;
}

// -----------------------------------------------------------
// FUNCTION: backend_close
// -----------------------------------------------------------
// -----------------------------------------------------------
// FUNCTION: backend_close
// -----------------------------------------------------------
void backend_close() {
    printf("\nClosing Node %c...\n", node_name);
    printf("Messages Sent: %d | Received: %d\n", sent_count, recv_count);
    printf("Logs saved to chat_log_%c.txt\n", node_name);
    close(sock);
} // <--- THIS IS THE MISSING BRACE
