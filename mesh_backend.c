#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include "mesh_backend.h"

#define MAX_NODES 26
#define MAX_IP_LEN 64
#define NODES_DATA_FILE "nodes.dat"

static int sock_fd = -1;
static char node_name = 'A';
static char history_file[64];
static struct sockaddr_in my_addr;
static int sent_count = 0;
static int recv_count = 0;

static pthread_mutex_t nodes_mutex = PTHREAD_MUTEX_INITIALIZER;

struct NodeInfo {
    char name;
    char ip[MAX_IP_LEN];
    int port;
};

static struct NodeInfo nodes[MAX_NODES];
static int node_count = 0;

static void get_time_str(char *buf, size_t size) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(buf, size, "%H:%M:%S", t);
}

static void write_history(const char *entry) {
    FILE *fp = fopen(history_file, "a");
    if (fp) { fprintf(fp, "%s\n", entry); fclose(fp); }
}

static void load_nodes_file(void) {
    FILE *fp = fopen(NODES_DATA_FILE, "r");
    if (!fp) { printf("nodes.dat not found!\n"); return; }
    node_count = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp) != NULL) {
        char n, ip[MAX_IP_LEN];
        int port;
        if (sscanf(line, " %c %63s %d", &n, ip, &port) == 3) {
            nodes[node_count].name = n;
            strncpy(nodes[node_count].ip, ip, MAX_IP_LEN - 1);
            nodes[node_count].ip[MAX_IP_LEN - 1] = '\0';
            nodes[node_count].port = port;
            node_count++;
        }
    }
    fclose(fp);
}

static void save_nodes_file(void) {
    FILE *fp = fopen(NODES_DATA_FILE, "w");
    if (!fp) return;
    for (int i = 0; i < node_count; i++)
        fprintf(fp, "%c %s %d\n", nodes[i].name, nodes[i].ip, nodes[i].port);
    fclose(fp);
}

static int find_node_index(char name) {
    for (int i = 0; i < node_count; i++)
        if (nodes[i].name == name) return i;
    return -1;
}

static void add_or_update_node(char name, const char *ip, int port) {
    pthread_mutex_lock(&nodes_mutex);
    int idx = find_node_index(name);
    if (idx >= 0) {
        strncpy(nodes[idx].ip, ip, MAX_IP_LEN - 1);
        nodes[idx].ip[MAX_IP_LEN - 1] = '\0';
        nodes[idx].port = port;
    } else if (node_count < MAX_NODES) {
        nodes[node_count].name = name;
        strncpy(nodes[node_count].ip, ip, MAX_IP_LEN - 1);
        nodes[node_count].ip[MAX_IP_LEN - 1] = '\0';
        nodes[node_count].port = port;
        node_count++;
    }
    save_nodes_file();
    pthread_mutex_unlock(&nodes_mutex);
}

static void send_welcome(char to_name) {
    pthread_mutex_lock(&nodes_mutex);
    int idx = find_node_index(to_name);
    if (idx < 0) { pthread_mutex_unlock(&nodes_mutex); return; }

    char payload[2048];
    int offset = snprintf(payload, sizeof(payload), "NET_WELCOME:");
    int truncated = 0;
    for (int i = 0; i < node_count; i++) {
        if (nodes[i].name == to_name) continue;
        /* Issue #2 fix: check remaining space before appending.
           Each entry is at most "Z:255.255.255.255:65535," = 26 chars. */
        if ((int)sizeof(payload) - offset < 30) {
            truncated = 1;
            break;
        }
        offset += snprintf(payload + offset, sizeof(payload) - offset,
                           "%c:%s:%d,", nodes[i].name, nodes[i].ip, nodes[i].port);
    }
    if (truncated)
        printf("WARNING: NET_WELCOME payload truncated — node %c will have incomplete map.\n", to_name);
    if (offset > 12 && payload[offset - 1] == ',') payload[offset - 1] = '\0';

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family      = AF_INET;
    dest.sin_port        = htons(nodes[idx].port);
    if (inet_pton(AF_INET, nodes[idx].ip, &dest.sin_addr) <= 0) {
        printf("ERROR: bad IP for node %c\n", to_name);
        pthread_mutex_unlock(&nodes_mutex);
        return;
    }
    pthread_mutex_unlock(&nodes_mutex);

    sendto(sock_fd, payload, strlen(payload), 0,
           (struct sockaddr *)&dest, sizeof(dest));
}

void backend_init(char name) {
    node_name = name;
    snprintf(history_file, sizeof(history_file), "chat_history_%c.txt", name);

    pthread_mutex_lock(&nodes_mutex);
    load_nodes_file();
    pthread_mutex_unlock(&nodes_mutex);

    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) { perror("socket"); exit(1); }

    memset(&my_addr, 0, sizeof(my_addr));
    my_addr.sin_family = AF_INET;

    pthread_mutex_lock(&nodes_mutex);
    int my_port = -1;
    for (int i = 0; i < node_count; i++)
        if (nodes[i].name == node_name) my_port = nodes[i].port;
    pthread_mutex_unlock(&nodes_mutex);

    if (my_port == -1) { printf("ERROR: Node %c not in nodes.dat\n", node_name); exit(1); }

    my_addr.sin_port        = htons(my_port);
    my_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock_fd, (struct sockaddr *)&my_addr, sizeof(my_addr)) < 0) {
        perror("bind"); exit(1);
    }

    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    printf("Backend started for %c on port %d\n", node_name, my_port);
    printf("Chat history will be saved to: %s\n", history_file);
}

void backend_send_message(char to, const char *msg) {
    pthread_mutex_lock(&nodes_mutex);
    int idx = find_node_index(to);
    if (idx < 0) { pthread_mutex_unlock(&nodes_mutex); printf("Unknown node %c\n", to); return; }

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family      = AF_INET;
    dest.sin_port        = htons(nodes[idx].port);
    if (inet_pton(AF_INET, nodes[idx].ip, &dest.sin_addr) <= 0) {
        printf("ERROR: bad IP for node %c\n", to);
        pthread_mutex_unlock(&nodes_mutex);
        return;
    }
    pthread_mutex_unlock(&nodes_mutex);

    char packet[1100];
    snprintf(packet, sizeof(packet), "%c|%s", node_name, msg);
    sendto(sock_fd, packet, strlen(packet), 0, (struct sockaddr *)&dest, sizeof(dest));
    sent_count++;

    char time_str[16];
    get_time_str(time_str, sizeof(time_str));
    char log_entry[1200];
    snprintf(log_entry, sizeof(log_entry), "[%s] Me -> %c: %s", time_str, to, msg);
    write_history(log_entry);
}

void backend_broadcast(const char *msg) {
    char packet[1100];
    snprintf(packet, sizeof(packet), "%c|(broadcasted) %s", node_name, msg);

    pthread_mutex_lock(&nodes_mutex);
    struct NodeInfo snapshot[MAX_NODES];
    int snap_count = node_count;
    memcpy(snapshot, nodes, sizeof(struct NodeInfo) * node_count);
    pthread_mutex_unlock(&nodes_mutex);

    for (int i = 0; i < snap_count; i++) {
        if (snapshot[i].name == node_name) continue;
        struct sockaddr_in dest;
        memset(&dest, 0, sizeof(dest));
        dest.sin_family      = AF_INET;
        dest.sin_port        = htons(snapshot[i].port);
        if (inet_pton(AF_INET, snapshot[i].ip, &dest.sin_addr) <= 0) continue;
        if (sendto(sock_fd, packet, strlen(packet), 0, (struct sockaddr *)&dest, sizeof(dest)) > 0)
            sent_count++;  /* Issue #3 fix: count each successful send, not just +1 for the whole broadcast */
    }

    char time_str[16];
    get_time_str(time_str, sizeof(time_str));
    char log_entry[1200];
    snprintf(log_entry, sizeof(log_entry), "[%s] Me -> ALL (Broadcast): %s", time_str, msg);
    write_history(log_entry);
}

int backend_receive(char *out, int max_len) {
    /* Bug fix #3: "From X at HH:MM:SS: " prefix is ~22 chars.
       Limit what we read so the assembled string always fits in out[max_len]. */
    #define MSG_PREFIX_OVERHEAD 25
    char buf[1024];
    int read_limit = max_len - MSG_PREFIX_OVERHEAD - 1;
    if (read_limit <= 0 || read_limit > 1023) read_limit = 1023;
    int bytes = recvfrom(sock_fd, buf, read_limit, 0, NULL, NULL);
    if (bytes < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    buf[bytes] = '\0';

    if (strncmp(buf, "NET_ADD:", 8) == 0) {
        char newName, newIP[64];
        int newPort;
        if (sscanf(buf + 8, "%c:%63[^:]:%d", &newName, newIP, &newPort) == 3) {
            add_or_update_node(newName, newIP, newPort);
            send_welcome(newName);
            printf("\n>>> Node %c joined (%s:%d)\n", newName, newIP, newPort);
            printf("Choose: "); fflush(stdout);
            char time_str[16]; get_time_str(time_str, sizeof(time_str));
            char log_entry[512];
            snprintf(log_entry, sizeof(log_entry),
                     "[%s] [SYSTEM] node %c joined %s:%d", time_str, newName, newIP, newPort);
            write_history(log_entry);
        }
        return 0;
    }

    if (strncmp(buf, "NET_WELCOME:", 12) == 0) {
        char *cursor = buf + 12;
        char entry[128];
        while (sscanf(cursor, "%127[^,]", entry) == 1) {
            char n, ip[64]; int port;
            if (sscanf(entry, "%c:%63[^:]:%d", &n, ip, &port) == 3)
                add_or_update_node(n, ip, port);
            cursor = strchr(cursor, ',');
            if (!cursor) break;
            cursor++;
        }
        printf("\n>>> Got network map — %d nodes known.\n", node_count);
        printf("Choose: "); fflush(stdout);
        return 0;
    }
    if (strncmp(buf, "NET_LEAVE:", 10) == 0) {
        char left_node = buf[10];
        pthread_mutex_lock(&nodes_mutex);
        int idx = find_node_index(left_node);
        if (idx >= 0) {
            for (int i = idx; i < node_count - 1; i++) {
                nodes[i] = nodes[i + 1];
            }
            node_count--;
            save_nodes_file();
        }
        pthread_mutex_unlock(&nodes_mutex);
        printf("\n>>> Node %c left the network\n", left_node);
        printf("Choose: "); fflush(stdout);
        char time_str[16]; get_time_str(time_str, sizeof(time_str));
        char log_entry[512];
        snprintf(log_entry, sizeof(log_entry),
                 "[%s] [SYSTEM] node %c left the network", time_str, left_node);
        write_history(log_entry);
        return 0;
    }

    char time_str[16];
    get_time_str(time_str, sizeof(time_str));
    char *sep = strchr(buf, '|');
    if (sep != NULL) {
        snprintf(out, max_len, "From %c at %s: %s", buf[0], time_str, sep + 1);
    } else {
        snprintf(out, max_len, "From ? at %s: %s", time_str, buf);
    }
    write_history(out);
    recv_count++;
    return bytes;
}
void backend_leave(void) {
    char packet[64];
    snprintf(packet, sizeof(packet), "NET_LEAVE:%c", node_name);
    pthread_mutex_lock(&nodes_mutex);
    for (int i = 0; i < node_count; i++) {
        if (nodes[i].name == node_name) continue;
        struct sockaddr_in dest;
        memset(&dest, 0, sizeof(dest));
        dest.sin_family      = AF_INET;
        dest.sin_port        = htons(nodes[i].port);
        if (inet_pton(AF_INET, nodes[i].ip, &dest.sin_addr) <= 0) continue;
        sendto(sock_fd, packet, strlen(packet), 0, (struct sockaddr *)&dest, sizeof(dest));
    }
    pthread_mutex_unlock(&nodes_mutex);
}

void backend_close(void) {
    /* Bug fix #4: guard against double-close. If called twice, or if the OS
       reuses the fd number for another resource, the second close() would
       silently corrupt an unrelated file descriptor. */
    if (sock_fd >= 0) {
        close(sock_fd);
        sock_fd = -1;
    }
}