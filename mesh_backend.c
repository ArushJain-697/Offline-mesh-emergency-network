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
#include "mesh_crypto.h"

static ssize_t encrypted_sendto(int fd, const char *buf, size_t len,
                                 const struct sockaddr *dest, socklen_t dest_len)
{
    uint8_t enc[2048];
    int enc_len = crypto_encrypt_packet((const uint8_t *)buf, (int)len,
                                         enc, sizeof(enc));
    if (enc_len < 0) return -1;
    return sendto(fd, enc, enc_len, 0, dest, dest_len);
}

static int encrypted_recvfrom(int fd, char *buf, int buf_len,
                               struct sockaddr *src, socklen_t *src_len)
{
    uint8_t raw[2048];
    int raw_n = recvfrom(fd, raw, sizeof(raw), 0, src, src_len);
    if (raw_n <= 0) return raw_n;
    int plain_n = crypto_decrypt_packet(raw, raw_n, (uint8_t *)buf, buf_len - 1);
    if (plain_n < 0) return 0; 
    buf[plain_n] = '\0';
    return plain_n;
}

#define MAX_NODES 26
#define MAX_IP_LEN 64
#define NODES_DATA_FILE "nodes.dat"

static int sock_fd = -1;
static char node_name = 'A';
static char history_file[64];
static int my_port_global = 0;  
static struct sockaddr_in my_addr;
static int sent_count = 0;
static int recv_count = 0;

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
    if (!fp) { return; }
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

    encrypted_sendto(sock_fd, payload, strlen(payload),
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

static char next_available_letter(void) {
    for (char c = 'A'; c <= 'Z'; c++) {
        if (find_node_index(c) < 0) return c;
    }
    return '\0';
}

static void on_new_peer_discovered(const char *ip, int port) {
    if (port == my_port_global) {
        return;
    }
    pthread_mutex_lock(&nodes_mutex);
    char letter = next_available_letter();
    if (letter == '\0') {
        pthread_mutex_unlock(&nodes_mutex);
        printf("[DISCOVERY] Network full, rejecting peer %s:%d\n", ip, port);
        return;
    }
    
    nodes[node_count].name = letter;
    strncpy(nodes[node_count].ip, ip, MAX_IP_LEN - 1);
    nodes[node_count].ip[MAX_IP_LEN - 1] = '\0';
    nodes[node_count].port = port;
    node_count++;
    save_nodes_file();
    pthread_mutex_unlock(&nodes_mutex);

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
            encrypted_sendto(sock_fd, add_pkt, strlen(add_pkt),
                   (struct sockaddr *)&d, sizeof(d));
    }

    send_welcome(letter);
    printf("\r\033[K>>> Node %c joined via discovery (%s:%d)\nChoose: ",
           letter, ip, port);
    fflush(stdout);
}

void backend_bootstrap(int port, const char *password, const char *helper_ip, int helper_port) {
    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) { perror("socket"); exit(1); }

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    my_port_global = port; 
    struct timeval tv = {1, 0};
    setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* ---- CHANGED: Crypto init now takes the password from CLI ---- */
    if (crypto_init(password) != 0) { fprintf(stderr, "Crypto init failed\n"); exit(1); }

    discovery_init();

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

    node_count = 0; 
    printf("[BOOTSTRAP] Searching for existing network on port %d...\n", port);
    char *welcome = discovery_bootstrap(sock_fd, port, helper_ip, helper_port);

    if (welcome == NULL) {
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
        pthread_mutex_lock(&nodes_mutex);
        node_count = 0;
        char *cursor = welcome + 12; 
        char entry[128];
        while (sscanf(cursor, "%127[^,]", entry) == 1) {
            char n, ip[64]; int p;
            if (sscanf(entry, "%c:%63[^:]:%d", &n, ip, &p) == 3) {
                nodes[node_count].name = n;
                strncpy(nodes[node_count].ip, ip, MAX_IP_LEN - 1);
                nodes[node_count].ip[MAX_IP_LEN - 1] = '\0';
                nodes[node_count].port = p;
                node_count++;
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

// ... [Keep backend_send_message, backend_broadcast, backend_receive, backend_leave, backend_close exactly the same] ...