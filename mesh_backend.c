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

static _Atomic int backend_running = 1;

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

/* --- NEW: Added last_seen timestamp --- */
struct NodeInfo {
    char name;
    char ip[MAX_IP_LEN];
    int port;
    time_t last_seen; 
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
    time_t now = time(NULL);
    while (fgets(line, sizeof(line), fp) != NULL) {
        char n, ip[MAX_IP_LEN];
        int port;
        if (sscanf(line, " %c %63s %d", &n, ip, &port) == 3) {
            nodes[node_count].name = n;
            strncpy(nodes[node_count].ip, ip, MAX_IP_LEN - 1);
            nodes[node_count].ip[MAX_IP_LEN - 1] = '\0';
            nodes[node_count].port = port;
            nodes[node_count].last_seen = now; /* Assume fresh on load */
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
    time_t now = time(NULL);
    if (idx >= 0) {
        strncpy(nodes[idx].ip, ip, MAX_IP_LEN - 1);
        nodes[idx].ip[MAX_IP_LEN - 1] = '\0';
        nodes[idx].port = port;
        nodes[idx].last_seen = now;
    } else if (node_count < MAX_NODES) {
        nodes[node_count].name = name;
        strncpy(nodes[node_count].ip, ip, MAX_IP_LEN - 1);
        nodes[node_count].ip[MAX_IP_LEN - 1] = '\0';
        nodes[node_count].port = port;
        nodes[node_count].last_seen = now;
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
        printf("WARNING: NET_WELCOME payload truncated.\n");
    if (offset > 12 && payload[offset - 1] == ',') payload[offset - 1] = '\0';

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family      = AF_INET;
    dest.sin_port        = htons(nodes[idx].port);
    if (inet_pton(AF_INET, nodes[idx].ip, &dest.sin_addr) <= 0) {
        pthread_mutex_unlock(&nodes_mutex);
        return;
    }
    pthread_mutex_unlock(&nodes_mutex);

    encrypted_sendto(sock_fd, payload, strlen(payload),
           (struct sockaddr *)&dest, sizeof(dest));
}

/* --- NEW: The Background Heartbeat & Purge Thread --- */
static void *heartbeat_loop(void *arg) {
    while (backend_running) {
        sleep(5); /* Beat every 5 seconds */
        time_t now = time(NULL);
        char hrt_pkt[32];
        snprintf(hrt_pkt, sizeof(hrt_pkt), "HRT:%c", node_name);

        pthread_mutex_lock(&nodes_mutex);
        for (int i = 0; i < node_count; ) {
            if (nodes[i].name == node_name) {
                nodes[i].last_seen = now; /* Never purge ourselves */
                i++;
                continue;
            }

            /* 1. Fire Heartbeat */
            struct sockaddr_in dest;
            memset(&dest, 0, sizeof(dest));
            dest.sin_family = AF_INET;
            dest.sin_port = htons(nodes[i].port);
            inet_pton(AF_INET, nodes[i].ip, &dest.sin_addr);
            encrypted_sendto(sock_fd, hrt_pkt, strlen(hrt_pkt), (struct sockaddr *)&dest, sizeof(dest));

            /* 2. Check for Death (15 seconds of silence) */
            if (now - nodes[i].last_seen > 15) {
                char dead_node = nodes[i].name;
                
                /* Shift array left to delete */
                for (int j = i; j < node_count - 1; j++) {
                    nodes[j] = nodes[j + 1];
                }
                node_count--;
                save_nodes_file();
                
                printf("\r\033[K[SYSTEM] Node %c timed out (Ghost Node removed).\nChoose: ", dead_node);
                fflush(stdout);
                
                char time_str[16]; get_time_str(time_str, sizeof(time_str));
                char log_entry[128];
                snprintf(log_entry, sizeof(log_entry), "[%s] [SYSTEM] Node %c timed out", time_str, dead_node);
                write_history(log_entry);
            } else {
                i++;
            }
        }
        pthread_mutex_unlock(&nodes_mutex);
    }
    return NULL;
}

static void start_heartbeat_thread(void) {
    pthread_t hb_tid;
    pthread_create(&hb_tid, NULL, heartbeat_loop, NULL);
    pthread_detach(hb_tid);
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
    start_heartbeat_thread();
}

static char next_available_letter(void) {
    for (char c = 'A'; c <= 'Z'; c++) {
        if (find_node_index(c) < 0) return c;
    }
    return '\0';
}

static void on_new_peer_discovered(const char *ip, int port) {
    if (port == my_port_global) return;

    pthread_mutex_lock(&nodes_mutex);
    /* Already known (re-knock, broadcast seen twice, or reconnect)? Just
       refresh liveness and re-welcome — never allocate a second letter. */
    for (int i = 0; i < node_count; i++) {
        if (strcmp(nodes[i].ip, ip) == 0 && nodes[i].port == port) {
            char known = nodes[i].name;
            nodes[i].last_seen = time(NULL);
            pthread_mutex_unlock(&nodes_mutex);
            send_welcome(known);
            return;
        }
    }

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
    nodes[node_count].last_seen = time(NULL);
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
            encrypted_sendto(sock_fd, add_pkt, strlen(add_pkt), (struct sockaddr *)&d, sizeof(d));
    }

    send_welcome(letter);
    printf("\r\033[K>>> Node %c joined via discovery (%s:%d)\nChoose: ", letter, ip, port);
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

    if (crypto_init(password) != 0) { fprintf(stderr, "Crypto init failed\n"); exit(1); }
    discovery_init();


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
        nodes[0].last_seen = time(NULL);
        node_count = 1;
        save_nodes_file();
        pthread_mutex_unlock(&nodes_mutex);
        printf("[BOOTSTRAP] No peers found. Starting as Genesis Node A (%s:%d)\n", my_ip, port);
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
                nodes[node_count].last_seen = time(NULL);
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
        printf("[BOOTSTRAP] Joined network as Node %c on port %d\n", node_name, port);
    }

    snprintf(history_file, sizeof(history_file), "chat_history_%c.txt", node_name);
    my_addr = addr;
    start_heartbeat_thread();
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
        pthread_mutex_unlock(&nodes_mutex); return;
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
        encrypted_sendto(sock_fd, packet, strlen(packet), (struct sockaddr *)&dest, sizeof(dest));
        
        for (int i = 0; i < 50; i++) {
            if (ack_received == 1) break;
            usleep(20 * 1000); 
        }
        
        if (ack_received == 1) break;
        attempts++;
        if (attempts < 3) {
            printf("\r\033[K[SYSTEM] Retransmitting to %c (Attempt %d)...\nChoose: ", to, attempts + 1);
            fflush(stdout);
        }
    }

    if (ack_received == 1) {
        send_seq[to_idx] = 1 - send_seq[to_idx];
        sent_count++;
        char time_str[16]; get_time_str(time_str, sizeof(time_str));
        char log_entry[1200];
        snprintf(log_entry, sizeof(log_entry), "[%s] Me -> %c: %s", time_str, to, msg);
        write_history(log_entry);
    } else {
        printf("\r\033[K[ERROR] Message delivery to %c failed. Node might be offline.\nChoose: ", to);
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
        if (encrypted_sendto(sock_fd, packet, strlen(packet), (struct sockaddr *)&dest, sizeof(dest)) > 0)
            sent_count++;  
    }

    char time_str[16]; get_time_str(time_str, sizeof(time_str));
    char log_entry[1200];
    snprintf(log_entry, sizeof(log_entry), "[%s] Me -> ALL (Broadcast): %s", time_str, msg);
    write_history(log_entry);
}

int backend_receive(char *out, int max_len) {
    #define MSG_PREFIX_OVERHEAD 25
    char buf[1024];
    int read_limit = max_len - MSG_PREFIX_OVERHEAD - 1;
    if (read_limit <= 0 || read_limit > 1023) read_limit = 1023;

    int dfd = discovery_get_fd();
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(sock_fd, &rfds);
    if (dfd >= 0) FD_SET(dfd, &rfds);
    int maxfd = (dfd > sock_fd ? dfd : sock_fd) + 1;
    struct timeval tv = {1, 0};
    int ready = select(maxfd, &rfds, NULL, NULL, &tv);
    if (ready <= 0) return 0;

    if (dfd >= 0 && FD_ISSET(dfd, &rfds)) {
        discovery_handle_incoming(on_new_peer_discovered);
        return 0;
    }

    /* --- NEW: Capture sender IP/Port so we can prove they are alive --- */
    struct sockaddr_in sender_addr;
    socklen_t sender_len = sizeof(sender_addr);
    int bytes = encrypted_recvfrom(sock_fd, buf, read_limit, (struct sockaddr *)&sender_addr, &sender_len);
    
    if (bytes < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    buf[bytes] = '\0';

    /* Mark sender as alive immediately */
    char sender_ip[64];
    inet_ntop(AF_INET, &sender_addr.sin_addr, sender_ip, sizeof(sender_ip));
    int sender_port = ntohs(sender_addr.sin_port);
    
    pthread_mutex_lock(&nodes_mutex);
    for (int i = 0; i < node_count; i++) {
        if (strcmp(nodes[i].ip, sender_ip) == 0 && nodes[i].port == sender_port) {
            nodes[i].last_seen = time(NULL);
            break;
        }
    }
    pthread_mutex_unlock(&nodes_mutex);

    /* Ignore Heartbeat packets so they don't spam the chat */
    if (strncmp(buf, "HRT:", 4) == 0) return 0;

    if (strncmp(buf, "NET_ADD:", 8) == 0) {
        char newName, newIP[64];
        int newPort;
        if (sscanf(buf + 8, "%c:%63[^:]:%d", &newName, newIP, &newPort) == 3) {
            add_or_update_node(newName, newIP, newPort);
            printf("\r\033[K>>> Node %c added to network (%s:%d)\nChoose: ", newName, newIP, newPort); fflush(stdout);
            char time_str[16]; get_time_str(time_str, sizeof(time_str));
            char log_entry[512];
            snprintf(log_entry, sizeof(log_entry), "[%s] [SYSTEM] node %c joined %s:%d", time_str, newName, newIP, newPort);
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
        printf("\r\033[K>>> Got network map — %d nodes known.\nChoose: ", node_count); fflush(stdout);
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
        printf("\r\033[K>>> Node %c left the network\nChoose: ", left_node); fflush(stdout);
        char time_str[16]; get_time_str(time_str, sizeof(time_str));
        char log_entry[512];
        snprintf(log_entry, sizeof(log_entry), "[%s] [SYSTEM] node %c left the network", time_str, left_node);
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
                encrypted_sendto(sock_fd, ack_pkt, strlen(ack_pkt), (struct sockaddr *)&dest, sizeof(dest));
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
                    return 0; 
                }
            }
        }
        return 0;
    }

    char time_str[16]; get_time_str(time_str, sizeof(time_str));
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
        encrypted_sendto(sock_fd, packet, strlen(packet), (struct sockaddr *)&dest, sizeof(dest));
    }
    pthread_mutex_unlock(&nodes_mutex);
}

void backend_close(void) {
    backend_running = 0; /* Tells the heartbeat thread to shut down cleanly */
    if (sock_fd >= 0) {
        close(sock_fd);
        sock_fd = -1;
    }
}