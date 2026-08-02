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
#include <sodium.h>
#include "mesh_backend.h"
#include "mesh_discovery.h"
#include "mesh_crypto.h"
#include "mesh_frame.h"

static _Atomic int backend_running = 1;

/* --- Bounded dedup cache: drop duplicate (relayed) and replayed packets ---
 * Fixed-size ring so memory never grows — required for the ESP32 target.
 * ponytail: O(n) linear scan over 256 ids; a Bloom filter only if this
 * ever shows up in a profile. */
#define DEDUP_CACHE 256
static uint8_t dedup_ids[DEDUP_CACHE][MESH_MSGID_BYTES];
static int dedup_pos = 0;
static int dedup_len = 0;
static pthread_mutex_t dedup_mutex = PTHREAD_MUTEX_INITIALIZER;

static void dedup_remember(const uint8_t *id) {
    pthread_mutex_lock(&dedup_mutex);
    memcpy(dedup_ids[dedup_pos], id, MESH_MSGID_BYTES);
    dedup_pos = (dedup_pos + 1) % DEDUP_CACHE;
    if (dedup_len < DEDUP_CACHE) dedup_len++;
    pthread_mutex_unlock(&dedup_mutex);
}

static int seen_recently(const uint8_t *id) {
    pthread_mutex_lock(&dedup_mutex);
    for (int i = 0; i < dedup_len; i++) {
        if (memcmp(dedup_ids[i], id, MESH_MSGID_BYTES) == 0) {
            pthread_mutex_unlock(&dedup_mutex);
            return 1;
        }
    }
    pthread_mutex_unlock(&dedup_mutex);
    dedup_remember(id);
    return 0;
}

/* Frame (binary header + payload) then encrypt then send. id==NULL mints a
 * fresh id (new-origin message); pass an id to preserve it when relaying. */
static ssize_t mesh_sendto(int fd, uint8_t ttl, const uint8_t *id,
                           const char *buf, size_t len,
                           const struct sockaddr *dest, socklen_t dest_len)
{
    uint8_t framed[2048];
    int fl = mesh_frame_encode(ttl, id, buf, (int)len, framed, sizeof(framed));
    if (fl < 0) return -1;

    uint8_t enc[2048];
    int enc_len = crypto_encrypt_packet(framed, fl, enc, sizeof(enc));
    if (enc_len < 0) return -1;
    return sendto(fd, enc, enc_len, 0, dest, dest_len);
}

static ssize_t encrypted_sendto(int fd, const char *buf, size_t len,
                                 const struct sockaddr *dest, socklen_t dest_len)
{
    return mesh_sendto(fd, MESH_DEFAULT_TTL, NULL, buf, len, dest, dest_len);
}

static int encrypted_recvfrom(int fd, char *buf, int buf_len, struct mesh_hdr *out_hdr,
                               struct sockaddr *src, socklen_t *src_len)
{
    uint8_t raw[2048];
    int raw_n = recvfrom(fd, raw, sizeof(raw), 0, src, src_len);
    if (raw_n <= 0) return raw_n;

    uint8_t plain[2048];
    int plain_n = crypto_decrypt_packet(raw, raw_n, plain, sizeof(plain));
    if (plain_n < 0) return 0;

    struct mesh_hdr hdr;
    const char *payload;
    int pl = mesh_frame_decode(plain, plain_n, &hdr, &payload);
    if (pl < 0) return 0;                    /* not a v1 frame — drop */
    if (seen_recently(hdr.msg_id)) return 0; /* duplicate or replay — drop */

    if (pl > buf_len - 1) pl = buf_len - 1;
    memcpy(buf, payload, pl);
    buf[pl] = '\0';
    if (out_hdr) *out_hdr = hdr;
    return pl;
}

#define MAX_NODES 26
#define MAX_IP_LEN 64
#define NODES_DATA_FILE "nodes.dat"

static int sock_fd = -1;
static char node_name = 'A';
static char history_file[64];
static int my_port_global = 0;
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

/* Flood a payload to every known peer under ONE shared msg_id, so relayers
 * across the mesh collapse all copies into a single flood tree via dedup.
 * Pre-remembers the id so our own packet never re-enters our handlers.
 * skip_ip/skip_port omits the neighbour we received a relayed packet from. */
static void flood_packet(const char *payload, uint8_t ttl, const uint8_t *id,
                         const char *skip_ip, int skip_port) {
    uint8_t local_id[MESH_MSGID_BYTES];
    if (!id) { randombytes_buf(local_id, sizeof(local_id)); id = local_id; }
    dedup_remember(id);

    pthread_mutex_lock(&nodes_mutex);
    struct NodeInfo snapshot[MAX_NODES];
    int snap_count = node_count;
    memcpy(snapshot, nodes, sizeof(struct NodeInfo) * node_count);
    pthread_mutex_unlock(&nodes_mutex);

    for (int i = 0; i < snap_count; i++) {
        if (snapshot[i].name == node_name) continue;
        if (skip_ip && snapshot[i].port == skip_port &&
            strcmp(snapshot[i].ip, skip_ip) == 0) continue;
        struct sockaddr_in d;
        memset(&d, 0, sizeof(d));
        d.sin_family = AF_INET;
        d.sin_port   = htons(snapshot[i].port);
        if (inet_pton(AF_INET, snapshot[i].ip, &d.sin_addr) <= 0) continue;
        mesh_sendto(sock_fd, ttl, id, payload, strlen(payload),
                    (struct sockaddr *)&d, sizeof(d));
    }
}

/* Topology + per-link heartbeat stay 1-hop; DIR/ACK/broadcast get relayed. */
static int is_relayable(const char *buf) {
    if (strncmp(buf, "HRT:", 4) == 0)          return 0;
    if (strncmp(buf, "NET_ADD:", 8) == 0)      return 0;
    if (strncmp(buf, "NET_WELCOME:", 12) == 0) return 0;
    if (strncmp(buf, "NET_LEAVE:", 10) == 0)   return 0;
    return 1;
}

/* --- Store-and-forward outbox -------------------------------------------
 * A disaster node drops off (power blip, radio fade) and comes back. Instead
 * of failing the send, queue it and flush when the node is reachable again.
 * Fixed-size ring (bounded memory for the ESP32 target); entries expire so a
 * permanently-dead node's backlog can't pin memory forever.
 * ponytail: keyed by node LETTER — correct while a node keeps its letter for
 * the session; stable cryptographic identity is the real fix (future). */
#define OUTBOX_MAX      32
#define OUTBOX_TTL_SEC  300      /* give up after 5 min undelivered */
#define FLUSH_PER_TICK  4        /* cap blocking sends per heartbeat beat */

struct OutboxMsg { char to; char msg[512]; time_t queued; int used; };
static struct OutboxMsg outbox[OUTBOX_MAX];
static pthread_mutex_t outbox_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t send_mutex   = PTHREAD_MUTEX_INITIALIZER; /* one ARQ txn at a time */

/* One stop-and-wait ARQ transaction. Returns 1 on ACK, 0 on give-up.
 * send_mutex serializes the shared ack_* state so a heartbeat-thread flush and
 * a main-thread send can't stomp each other. */
static int deliver(char to, const char *msg) {
    int to_idx = to - 'A';
    if (to_idx < 0 || to_idx >= MAX_NODES) return 0;

    pthread_mutex_lock(&send_mutex);
    char packet[1100];
    int seq = send_seq[to_idx];
    snprintf(packet, sizeof(packet), "DIR:%c:%d:%c:%s", to, seq, node_name, msg);

    ack_expected = seq;
    ack_from     = to;
    ack_received = 0;

    int attempts = 0, ok = 0;
    while (attempts < 3) {
        flood_packet(packet, MESH_DEFAULT_TTL, NULL, NULL, 0);
        for (int i = 0; i < 50; i++) {
            if (ack_received == 1) break;
            usleep(20 * 1000);
        }
        if (ack_received == 1) { ok = 1; break; }
        attempts++;
        if (attempts < 3) {
            printf("\r\033[K[SYSTEM] Retransmitting to %c (Attempt %d)...\nChoose: ", to, attempts + 1);
            fflush(stdout);
        }
    }
    if (ok) {
        send_seq[to_idx] = 1 - send_seq[to_idx];
        sent_count++;
        char time_str[16]; get_time_str(time_str, sizeof(time_str));
        char log_entry[1200];
        snprintf(log_entry, sizeof(log_entry), "[%s] Me -> %c: %s", time_str, to, msg);
        write_history(log_entry);
    }
    pthread_mutex_unlock(&send_mutex);
    return ok;
}

static void outbox_enqueue(char to, const char *msg) {
    pthread_mutex_lock(&outbox_mutex);
    int slot = -1;
    for (int i = 0; i < OUTBOX_MAX; i++)
        if (!outbox[i].used) { slot = i; break; }
    if (slot < 0) {                       /* full: overwrite the oldest entry */
        slot = 0;
        for (int i = 1; i < OUTBOX_MAX; i++)
            if (outbox[i].queued < outbox[slot].queued) slot = i;
    }
    outbox[slot].to = to;
    strncpy(outbox[slot].msg, msg, sizeof(outbox[slot].msg) - 1);
    outbox[slot].msg[sizeof(outbox[slot].msg) - 1] = '\0';
    outbox[slot].queued = time(NULL);
    outbox[slot].used = 1;
    pthread_mutex_unlock(&outbox_mutex);
}

/* Attempt queued deliveries to nodes currently in the registry. Runs on the
 * heartbeat thread (NOT holding nodes_mutex) so deliver()'s ACK wait doesn't
 * block the receiver thread. */
static void outbox_flush(void) {
    time_t now = time(NULL);
    int done = 0;
    for (int i = 0; i < OUTBOX_MAX && done < FLUSH_PER_TICK; i++) {
        pthread_mutex_lock(&outbox_mutex);
        if (!outbox[i].used) { pthread_mutex_unlock(&outbox_mutex); continue; }
        if (now - outbox[i].queued > OUTBOX_TTL_SEC) {   /* expired — drop */
            outbox[i].used = 0;
            pthread_mutex_unlock(&outbox_mutex);
            printf("\r\033[K[SYSTEM] Queued message to %c expired undelivered.\nChoose: ", outbox[i].to);
            fflush(stdout);
            continue;
        }
        char to = outbox[i].to;
        char msg[512];
        strncpy(msg, outbox[i].msg, sizeof(msg));
        msg[sizeof(msg) - 1] = '\0';
        pthread_mutex_unlock(&outbox_mutex);

        pthread_mutex_lock(&nodes_mutex);
        int known = find_node_index(to) >= 0;
        pthread_mutex_unlock(&nodes_mutex);
        if (!known) continue;             /* still gone — keep it queued */

        done++;
        if (deliver(to, msg)) {
            pthread_mutex_lock(&outbox_mutex);
            outbox[i].used = 0;
            pthread_mutex_unlock(&outbox_mutex);
            printf("\r\033[K[SYSTEM] Queued message to %c delivered.\nChoose: ", to);
            fflush(stdout);
        }
    }
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

        outbox_flush();   /* retry queued mail to any node that's back */
    }
    return NULL;
}

static void start_heartbeat_thread(void) {
    pthread_t hb_tid;
    pthread_create(&hb_tid, NULL, heartbeat_loop, NULL);
    pthread_detach(hb_tid);
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
    start_heartbeat_thread();
}

void backend_send_message(char to, const char *msg) {
    int to_idx = to - 'A';
    if (to_idx < 0 || to_idx >= MAX_NODES) { printf("Unknown node %c\n", to); return; }

    /* Multi-hop unicast = controlled flood carrying an explicit destination.
     * Every node relays it; only `to` consumes and ACKs. No routing table. */
    if (deliver(to, msg)) return;

    /* Unreachable now — hold it and let the heartbeat flush retry when the
     * node returns, instead of losing the message. */
    outbox_enqueue(to, msg);
    printf("\r\033[K[SYSTEM] %c unreachable — queued for delivery when it returns.\nChoose: ", to);
    fflush(stdout);
}

void backend_broadcast(const char *msg) {
    char packet[1100];
    snprintf(packet, sizeof(packet), "%c|(broadcasted) %s", node_name, msg);

    /* Flood to the whole mesh (TTL-bounded) — the disaster alert path. */
    flood_packet(packet, MESH_DEFAULT_TTL, NULL, NULL, 0);
    sent_count++;

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
    struct mesh_hdr hdr = {0};
    int bytes = encrypted_recvfrom(sock_fd, buf, read_limit, &hdr,
                                   (struct sockaddr *)&sender_addr, &sender_len);

    if (bytes < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    if (bytes == 0) return 0;   /* decrypt fail, bad frame, or dedup drop */
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

    /* Multi-hop relay: forward not-yet-seen data packets one hop closer to
     * exhaustion. Dedup guarantees we relay each packet at most once, which is
     * exactly what bounds the flood. */
    if (hdr.ttl > 1 && is_relayable(buf))
        flood_packet(buf, hdr.ttl - 1, hdr.msg_id, sender_ip, sender_port);

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
        char dst; int seq; char sender;
        if (sscanf(buf + 4, "%c:%d:%c", &dst, &seq, &sender) == 3) {
            if (dst != node_name) return 0;   /* not our ACK — relay handled it */
            if (sender == ack_from && seq == ack_expected) {
                ack_received = 1;
            }
        }
        return 0;
    }

    if (strncmp(buf, "DIR:", 4) == 0) {
        char dst; int seq; char sender;
        char msg_content[1024];
        if (sscanf(buf + 4, "%c:%d:%c:%1023[^\n]", &dst, &seq, &sender, msg_content) == 4) {
            if (dst != node_name) return 0;   /* not for us — relay already forwarded it */

            /* ACK back to the sender, flooded so it survives the return path. */
            char ack_pkt[32];
            snprintf(ack_pkt, sizeof(ack_pkt), "ACK:%c:%d:%c", sender, seq, node_name);
            flood_packet(ack_pkt, MESH_DEFAULT_TTL, NULL, NULL, 0);

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