// mesh_backend.c
// Features: Auto-Accept + Sender ID + Timestamp + Broadcast Tag + Chat History File
// Update: "New node joined" detailed message

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <time.h>           
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>

#include "mesh_backend.h"

#define MAX_NODES 26
#define MAX_IP_LEN 64
#define NODES_DATA_FILE "nodes.dat"

static int sock_fd = -1;
static char node_name = 'A';
static char history_file[64]; // Filename for chat history

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

/* ---------------- HELPER: Get Current Time String ---------------- */
static void get_time_str(char *buf, size_t size) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(buf, size, "%H:%M:%S", t);
}

/* ---------------- HELPER: Write to History File ---------------- */
static void write_history(const char *entry) {
    FILE *fp = fopen(history_file, "a"); // "a" means append
    if (fp) {
        fprintf(fp, "%s\n", entry);
        fclose(fp);
    }
}

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
    
    // Set the history filename, e.g., "chat_history_A.txt"
    snprintf(history_file, sizeof(history_file), "chat_history_%c.txt", name);

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
    printf("Chat history will be saved to: %s\n", history_file);
}

/* ---------------- send message ---------------- */
void backend_send_message(char to, const char *msg) {
    int idx = find_node_index(to);
    if (idx < 0) { printf("Unknown node %c\n", to); return; }

    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(nodes[idx].port);
    dest_addr.sin_addr.s_addr = inet_addr(nodes[idx].ip);

    // Prepare packet: "Sender|Message"
    char packet[1100];
    snprintf(packet, sizeof(packet), "%c|%s", node_name, msg);

    sendto(sock_fd, packet, strlen(packet), 0,
        (struct sockaddr *)&dest_addr, sizeof(dest_addr));

    sent_count++;

    // LOG TO FILE
    char time_str[16];
    get_time_str(time_str, sizeof(time_str));
    char log_entry[1200];
    snprintf(log_entry, sizeof(log_entry), "[%s] Me -> %c: %s", time_str, to, msg);
    write_history(log_entry);
}

/* ---------------- broadcast ---------------- */
void backend_broadcast(const char *msg) {
    // Prepare packet: "Sender|(broadcasted) Message"
    char packet[1100];
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

    // LOG TO FILE
    char time_str[16];
    get_time_str(time_str, sizeof(time_str));
    char log_entry[1200];
    snprintf(log_entry, sizeof(log_entry), "[%s] Me -> ALL (Broadcast): %s", time_str, msg);
    write_history(log_entry);
}

/* ---------------- receive ---------------- */
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
            
            // 1. Prepare the detailed message string
            char msg[256];
            snprintf(msg, sizeof(msg), "new node %c has joined, its port is %d, ip %s", 
                     newName, newPort, newIP);

            printf("%s\n", msg);
            
            // Auto-accept
            add_or_update_node(newName, newIP, newPort);
            printf(">>> Auto-accepted node %c into mesh.\n", newName);
            printf("Choose: ");
            fflush(stdout);

            // 2. Log System Event with timestamp
            char time_str[16];
            get_time_str(time_str, sizeof(time_str));
            char log_entry[512];
            snprintf(log_entry, sizeof(log_entry), "[%s] [SYSTEM] %s", time_str, msg);
            write_history(log_entry);

        } else {
            printf("Malformed NET_ADD message.\n");
        }

        return 0; // consumed internally
    }

    /* ---------------- NORMAL MESSAGE LOGIC ---------------- */
    
    // 1. Get Time
    char time_str[16];
    get_time_str(time_str, sizeof(time_str));

    // 2. Check for Sender Header "X|Message"
    char *sep = strchr(buf, '|');
    if (sep != NULL) {
        char sender_char = buf[0];
        char *actual_msg = sep + 1;

        // Format for Display and Log
        snprintf(out, max_len, "From %c at %s: %s", sender_char, time_str, actual_msg);
    } 
    else {
        // Fallback
        snprintf(out, max_len, "From ? at %s: %s", time_str, buf);
    }

    // LOG TO FILE (Log exactly what is shown on screen)
    write_history(out);

    recv_count++;
    return bytes;
}

/* ---------------- close ---------------- */
void backend_close(void) {
    close(sock_fd);
}