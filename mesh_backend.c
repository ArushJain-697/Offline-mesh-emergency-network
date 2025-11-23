// mesh_backend.c
// Features: Auto-Accept Nodes + Sender ID + Timestamp

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <time.h>           // Added for timestamp
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>

#include "mesh_backend.h"

#define MAX_NODES 26
#define MAX_IP_LEN 64
#define NODES_DATA_FILE "nodes.dat"

static int sock_fd = -1;
static char node_name = 'A';
static struct sockaddr_in my_addr;
static struct sockaddr_in dest_addr;

static int sent_count = 0;
static int recv_count = 0;

struct NodeInfo {
    char name;
    char ip[MAX_IP_LEN];
    int port;
};

static struct NodeInfo nodes[MAX_NODES];
static int node_count = 0;


/* ---------------- load nodes.dat ---------------- */
static void load_nodes_file(void) {
    FILE *fp = fopen(NODES_DATA_FILE, "r");
    char line[256];

    if (!fp) {
        printf("nodes.dat not found! Create it manually.\n");
        return;
    }

    node_count = 0;

    while (fgets(line, sizeof(line), fp) != NULL) {
        char n, ip[MAX_IP_LEN];
        int port;

        if (sscanf(line, " %c %63s %d", &n, ip, &port) == 3) {
            nodes[node_count].name = n;
            strncpy(nodes[node_count].ip, ip, MAX_IP_LEN);
            nodes[node_count].port = port;
            node_count++;
        }
    }
    fclose(fp);
}


/* ---------------- save nodes.dat ---------------- */
static void save_nodes_file(void) {
    FILE *fp = fopen(NODES_DATA_FILE, "w");
    if (!fp) return;

    for (int i = 0; i < node_count; i++) {
        fprintf(fp, "%c %s %d\n",
            nodes[i].name,
            nodes[i].ip,
            nodes[i].port);
    }

    fclose(fp);
}


/* ---------------- find node ---------------- */
static int find_node_index(char name) {
    for (int i = 0; i < node_count; i++)
        if (nodes[i].name == name)
            return i;
    return -1;
}


/* ---------------- add/update node ---------------- */
static void add_or_update_node(char name, const char *ip, int port) {
    int idx = find_node_index(name);

    if (idx >= 0) {
        strncpy(nodes[idx].ip, ip, MAX_IP_LEN);
        nodes[idx].port = port;
    }
    else if (node_count < MAX_NODES) {
        nodes[node_count].name = name;
        strncpy(nodes[node_count].ip, ip, MAX_IP_LEN);
        nodes[node_count].port = port;
        node_count++;
    }

    save_nodes_file();
}


/* ---------------- init backend ---------------- */
void backend_init(char name) {
    node_name = name;

    load_nodes_file();

    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) { perror("socket"); exit(1); }

    memset(&my_addr, 0, sizeof(my_addr));
    my_addr.sin_family = AF_INET;

    int my_port = -1;
    for (int i = 0; i < node_count; i++)
        if (nodes[i].name == node_name)
            my_port = nodes[i].port;

    if (my_port == -1) {
        printf("ERROR: Node %c not in nodes.dat\n", node_name);
        exit(1);
    }

    my_addr.sin_port = htons(my_port);
    my_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock_fd, (struct sockaddr *)&my_addr, sizeof(my_addr)) < 0) {
        perror("bind"); exit(1);
    }

    /* timeout so recvfrom() does not block forever */
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    printf("Backend started for %c on port %d\n", node_name, my_port);
}


/* ---------------- send message (UPDATED) ---------------- */
void backend_send_message(char to, const char *msg) {
    int idx = find_node_index(to);
    if (idx < 0) { printf("Unknown node %c\n", to); return; }

    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(nodes[idx].port);
    dest_addr.sin_addr.s_addr = inet_addr(nodes[idx].ip);

    // NEW: Prepare packet as "Sender|Message"
    char packet[1100];
    snprintf(packet, sizeof(packet), "%c|%s", node_name, msg);

    sendto(sock_fd, packet, strlen(packet), 0,
        (struct sockaddr *)&dest_addr, sizeof(dest_addr));

    sent_count++;
}


/* ---------------- broadcast (UPDATED) ---------------- */
/* ---------------- broadcast (UPDATED) ---------------- */
void backend_broadcast(const char *msg) {
    char packet[1100];

    // FIX: We prepend "(broadcasted)" to the message here.
    // The receiver will see: "From A at 14:30: (broadcasted) Hello"
    snprintf(packet, sizeof(packet), "%c|(broadcasted) %s", node_name, msg);

    for (int i = 0; i < node_count; i++) {
        if (nodes[i].name == node_name) continue;

        memset(&dest_addr, 0, sizeof(dest_addr));
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(nodes[i].port);
        dest_addr.sin_addr.s_addr = inet_addr(nodes[i].ip);

        sendto(sock_fd, packet, strlen(packet), 0,
            (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    }
    sent_count++;
}


/* ---------------- receive (UPDATED) ---------------- */
int backend_receive(char *out, int max_len) {
    char buf[1024];

    int bytes = recvfrom(sock_fd, buf, 1023, 0, NULL, NULL);

    if (bytes < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;  
        return -1;
    }

    buf[bytes] = 0;

    /* ------------------ NET_ADD LOGIC ------------------ */
    if (strncmp(buf, "NET_ADD:", 8) == 0) {
        char newName, newIP[64];
        int newPort;

        if (sscanf(buf + 8, "%c:%63[^:]:%d", &newName, newIP, &newPort) == 3) {
            printf("\n*** NEW NODE REQUEST DETECTED ***\n");
            printf("Node: %c | IP: %s | Port: %d\n", newName, newIP, newPort);
            
            // Auto-accept (No inputs allowed here)
            add_or_update_node(newName, newIP, newPort);
            printf(">>> Auto-accepted node %c into mesh.\n", newName);
            printf("Choose: ");
            fflush(stdout);

        } else {
            printf("Malformed NET_ADD message.\n");
        }

        return 0; // consumed internally
    }

    /* ---------------- NORMAL MESSAGE LOGIC ---------------- */
    
    // 1. Get Current Time
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char time_str[16];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", t);

    // 2. Check for Sender Header "X|Message"
    char *sep = strchr(buf, '|');
    if (sep != NULL) {
        // We found the separator!
        char sender_char = buf[0];
        char *actual_msg = sep + 1;

        // Format: "From A at 14:30: Hello"
        snprintf(out, max_len, "From %c at %s: %s", sender_char, time_str, actual_msg);
    } 
    else {
        // Old format fallback (if unknown sender)
        snprintf(out, max_len, "From ? at %s: %s", time_str, buf);
    }

    recv_count++;
    return bytes;
}


/* ---------------- close ---------------- */
void backend_close(void) {
    close(sock_fd);
}