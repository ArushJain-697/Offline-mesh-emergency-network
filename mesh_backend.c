#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include "mesh_backend.h"
#include "mesh_discovery.h"

#define MAX_NODES 26
#define MAX_IP_LEN 64
#define NODES_DATA_FILE "nodes.dat"

static int sock_fd = -1;
static char node_name = 'A';
static char history_file[64];
static struct sockaddr_in my_addr;
static int sent_count = 0;
static int recv_count = 0;

/* RDT 3.0 State Tracking */
static int send_seq[MAX_NODES] = {0};
static int expected_seq[MAX_NODES] = {0};
static _Atomic int ack_received = 0;
static _Atomic int ack_expected = 0;
static _Atomic char ack_from = '\0';

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

/* ── Callback registered with mesh_discovery ──────────────────────
   Called when a NET_DISCOVER packet arrives on disc_fd.
   Assigns the next available letter, adds the peer, broadcasts
   NET_ADD to everyone, then sends NET_WELCOME back.            */
static char next_available_letter(void) {
    for (char c = 'A'; c <= 'Z'; c++) {
        if (find_node_index(c) < 0) return c;
    }
    return '\0';
}

static void on_new_peer_discovered(const char *ip, int port) {
    pthread_mutex_lock(&nodes_mutex);
    char letter = next_available_letter();
    if (letter == '\0') {
        pthread_mutex_unlock(&nodes_mutex);
        printf("[DISCOVERY] Network full, rejecting peer %s:%d\n", ip, port);
        return;
    }
    /* Add to local registry */
    nodes[node_count].name = letter;
    strncpy(nodes[node_count].ip, ip, MAX_IP_LEN - 1);
    nodes[node_count].ip[MAX_IP_LEN - 1] = '\0';
    nodes[node_count].port = port;
    node_count++;
    save_nodes_file();
    pthread_mutex_unlock(&nodes_mutex);

    /* Broadcast NET_ADD to all existing peers */
    char add_pkt[128];
    snprintf(add_pkt, sizeof(add_pkt), "NET_ADD:%c:%s:%d", letter, ip, port);
    struct NodeInfo snapshot[MAX_NODES];
    int snap_count;
    pthread_mutex_lock(&nodes_mutex);
    snap_count = node_count;
    memcpy(snapshot, nodes, sizeof(struct NodeInfo) * node_count);
    pthread_mutex_unlock(&nodes_mutex);
    for (int i = 0; i < snap_count; i++) {
        if (snapshot[i].name == letter || snapshot[i].name == node_name) continue;
        struct sockaddr_in d = {0};
        d.sin_family = AF_INET;
        d.sin_port   = htons(snapshot[i].port);
        if (inet_pton(AF_INET, snapshot[i].ip, &d.sin_addr) > 0)
            sendto(sock_fd, add_pkt, strlen(add_pkt), 0,
                   (struct sockaddr *)&d, sizeof(d));
    }

    /* Send full network map back to the new peer */
    send_welcome(letter);
    printf("\n>>> Node %c joined via discovery (%s:%d)\nChoose: ",
           letter, ip, port);
    fflush(stdout);
}

void backend_bootstrap(int port, const char *helper_ip, int helper_port) {
    /* Step 1: Open personal chat socket */
    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) { perror("socket"); exit(1); }

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    struct timeval tv = {1, 0};
    setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Step 2: Open discovery socket (port 9000) */
    discovery_init();

    /* ── Backward-compat fast path ─────────────────────────────────
       If nodes.dat exists AND already contains an entry for our port,
       we are a known node (e.g. a Docker container with baked-in dat).
       Skip all discovery, load the file, and start immediately.     */
    pthread_mutex_lock(&nodes_mutex);
    load_nodes_file();
    int found_self = 0;
    for (int i = 0; i < node_count; i++) {
        if (nodes[i].port == port) {
            node_name = nodes[i].name;
            found_self = 1;
            break;
        }
    }
    pthread_mutex_unlock(&nodes_mutex);

    if (found_self) {
        snprintf(history_file, sizeof(history_file),
                 "chat_history_%c.txt", node_name);
        my_addr = addr;
        printf("[BOOTSTRAP] Found self in nodes.dat as Node %c on port %d"
               " — skipping discovery.\n", node_name, port);
        printf("Chat history will be saved to: %s\n", history_file);
        return;
    }

    /* No existing nodes.dat entry — run discovery */
    node_count = 0; /* reset stale data from partial load */
    printf("[BOOTSTRAP] Searching for existing network on port %d...\n", port);
    char *welcome = discovery_bootstrap(sock_fd, port, helper_ip, helper_port);

    if (welcome == NULL) {
        /* Nobody answered → become Genesis node */
        char my_ip[MAX_IP_LEN];
        discovery_get_my_ip(my_ip, sizeof(my_ip));
        node_name = 'A';
        pthread_mutex_lock(&nodes_mutex);
        nodes[0].name = 'A';
        strncpy(nodes[0].ip, my_ip, MAX_IP_LEN - 1);
        nodes[0].port = port;
        node_count = 1;
        save_nodes_file();
        pthread_mutex_unlock(&nodes_mutex);
        printf("[BOOTSTRAP] No peers found. Starting as Genesis Node A (%s:%d)\n",
               my_ip, port);
    } else {
        /* Parse NET_WELCOME to build nodes[] and find own letter */
        pthread_mutex_lock(&nodes_mutex);
        node_count = 0;
        char *cursor = welcome + 12; /* skip "NET_WELCOME:" */
        char entry[128];
        while (sscanf(cursor, "%127[^,]", entry) == 1) {
            char n, ip[64]; int p;
            if (sscanf(entry, "%c:%63[^:]:%d", &n, ip, &p) == 3) {
                nodes[node_count].name = n;
                strncpy(nodes[node_count].ip, ip, MAX_IP_LEN - 1);
                nodes[node_count].ip[MAX_IP_LEN - 1] = '\0';
                nodes[node_count].port = p;
                node_count++;
                /* If this entry matches our port → we are this node */
                if (p == port) node_name = n;
            }
            cursor = strchr(cursor, ',');
            if (!cursor) break;
            cursor++;
        }
        save_nodes_file();
        pthread_mutex_unlock(&nodes_mutex);
        free(welcome);
        printf("[BOOTSTRAP] Joined network as Node %c on port %d\n",
               node_name, port);
    }

    snprintf(history_file, sizeof(history_file),
             "chat_history_%c.txt", node_name);
    my_addr = addr;
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
    int to_idx = to - 'A';
    if (to_idx < 0 || to_idx >= MAX_NODES) return;
    int seq = send_seq[to_idx];
    snprintf(packet, sizeof(packet), "DIR:%d:%c:%s", seq, node_name, msg);

    ack_expected = seq;
    ack_from = to;
    ack_received = 0;

    int attempts = 0;
    while (attempts < 3) {
        sendto(sock_fd, packet, strlen(packet), 0, (struct sockaddr *)&dest, sizeof(dest));
        
        for (int i = 0; i < 50; i++) {
            if (ack_received == 1) break;
            usleep(20 * 1000); // Wait 20ms chunks (total 1 second per attempt)
        }
        
        if (ack_received == 1) break;
        attempts++;
        if (attempts < 3) {
            printf("\n[SYSTEM] Retransmitting to %c (Attempt %d)...\nChoose: ", to, attempts + 1);
            fflush(stdout);
        }
    }

    if (ack_received == 1) {
        send_seq[to_idx] = 1 - send_seq[to_idx];
        sent_count++;
        char time_str[16];
        get_time_str(time_str, sizeof(time_str));
        char log_entry[1200];
        snprintf(log_entry, sizeof(log_entry), "[%s] Me -> %c: %s", time_str, to, msg);
        write_history(log_entry);
    } else {
        printf("\n[ERROR] Message delivery to %c failed. Node might be offline.\nChoose: ", to);
        fflush(stdout);
    }
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
    #define MSG_PREFIX_OVERHEAD 25
    char buf[1024];
    int read_limit = max_len - MSG_PREFIX_OVERHEAD - 1;
    if (read_limit <= 0 || read_limit > 1023) read_limit = 1023;

    /* Use select() to watch both the chat socket and discovery socket */
    int dfd = discovery_get_fd();
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(sock_fd, &rfds);
    if (dfd >= 0) FD_SET(dfd, &rfds);
    int maxfd = (dfd > sock_fd ? dfd : sock_fd) + 1;
    struct timeval tv = {1, 0};
    int ready = select(maxfd, &rfds, NULL, NULL, &tv);
    if (ready <= 0) return 0;

    /* Discovery socket has data → new node is knocking */
    if (dfd >= 0 && FD_ISSET(dfd, &rfds)) {
        discovery_handle_incoming(on_new_peer_discovered);
        return 0;
    }

    /* Chat socket has data */
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

    if (strncmp(buf, "ACK:", 4) == 0) {
        int seq; char sender;
        if (sscanf(buf + 4, "%d:%c", &seq, &sender) == 2) {
            if (sender == ack_from && seq == ack_expected) {
                ack_received = 1;
            }
        }
        return 0;
    }

    if (strncmp(buf, "DIR:", 4) == 0) {
        int seq; char sender;
        char msg_content[1024];
        if (sscanf(buf + 4, "%d:%c:%1023[^\n]", &seq, &sender, msg_content) == 3) {
            char ack_pkt[32];
            snprintf(ack_pkt, sizeof(ack_pkt), "ACK:%d:%c", seq, node_name);
            pthread_mutex_lock(&nodes_mutex);
            int idx = find_node_index(sender);
            if (idx >= 0) {
                struct sockaddr_in dest;
                memset(&dest, 0, sizeof(dest));
                dest.sin_family = AF_INET;
                dest.sin_port = htons(nodes[idx].port);
                inet_pton(AF_INET, nodes[idx].ip, &dest.sin_addr);
                sendto(sock_fd, ack_pkt, strlen(ack_pkt), 0, (struct sockaddr *)&dest, sizeof(dest));
            }
            pthread_mutex_unlock(&nodes_mutex);
            
            int sender_idx = sender - 'A';
            if (sender_idx >= 0 && sender_idx < MAX_NODES) {
                if (seq == expected_seq[sender_idx]) {
                    expected_seq[sender_idx] = 1 - expected_seq[sender_idx];
                    char time_str[16]; get_time_str(time_str, sizeof(time_str));
                    snprintf(out, max_len, "From %c at %s: %s", sender, time_str, msg_content);
                    write_history(out);
                    recv_count++;
                    return strlen(out);
                } else {
                    return 0; /* Duplicate message, ACK was sent but we ignore the payload */
                }
            }
        }
        return 0;
    }

    /* Fallback for broadcasts which still use the old A|msg format */
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