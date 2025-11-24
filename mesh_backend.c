// mesh_backend.c
// Auto-IP assignment backend; distributes nodes.dat updates across the LAN
// Uses UDP sockets. Nodes file format (nodes.dat):
// A 10.138.204.116
// B 0.0.0.0
// C 0.0.0.0
// D 0.0.0.0
// E 0.0.0.0

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <time.h>
#include <pthread.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>

#include "mesh_backend.h"

#define NODES_FILE "nodes.dat"
#define MAX_LINE 128
#define NODES_COUNT 5

static const int node_ports[NODES_COUNT] = {9001, 9002, 9003, 9004, 9005};

static int sockfd = -1;
static struct sockaddr_in my_addr;
static char local_node = 'A';
static char local_ip[64] = "0.0.0.0";

struct NodeSlot {
    char node;
    char ip[64];
};
static struct NodeSlot nodes[NODES_COUNT];

static pthread_t receiver_thread;
static volatile int running = 0;
static pthread_mutex_t file_lock = PTHREAD_MUTEX_INITIALIZER;

// -------------------- Utilities --------------------

static int node_index_from_letter(char node) {
    if (node < 'A' || node > 'E') return -1;
    return node - 'A';
}

static void init_nodes_default(void) {
    for (int i = 0; i < NODES_COUNT; ++i) {
        nodes[i].node = 'A' + i;
        strncpy(nodes[i].ip, "0.0.0.0", sizeof(nodes[i].ip)-1);
    }
}

// Read nodes.dat with format: "A 10.1.2.3"
static void load_nodes_file(void) {
    pthread_mutex_lock(&file_lock);
    FILE *f = fopen(NODES_FILE, "r");
    init_nodes_default();
    if (!f) {
        // If file missing, create default file
        FILE *g = fopen(NODES_FILE, "w");
        if (g) {
            for (int i = 0; i < NODES_COUNT; ++i) {
                fprintf(g, "%c %s\n", nodes[i].node, nodes[i].ip);
            }
            fclose(g);
        }
        pthread_mutex_unlock(&file_lock);
        return;
    }
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        char nodech;
        char ipbuf[64];
        if (sscanf(line, " %c %63s", &nodech, ipbuf) == 2) {
            int idx = node_index_from_letter(toupper(nodech));
            if (idx >= 0 && idx < NODES_COUNT) {
                nodes[idx].node = toupper(nodech);
                strncpy(nodes[idx].ip, ipbuf, sizeof(nodes[idx].ip)-1);
            }
        }
    }
    fclose(f);
    pthread_mutex_unlock(&file_lock);
}

// Save nodes array to nodes.dat (atomic write)
static void save_nodes_file(void) {
    pthread_mutex_lock(&file_lock);
    char tmpname[64];
    snprintf(tmpname, sizeof(tmpname), "%s.tmp", NODES_FILE);
    FILE *f = fopen(tmpname, "w");
    if (!f) { pthread_mutex_unlock(&file_lock); return; }
    for (int i = 0; i < NODES_COUNT; ++i) {
        fprintf(f, "%c %s\n", nodes[i].node, nodes[i].ip);
    }
    fflush(f);
    fclose(f);
    // atomically replace
    rename(tmpname, NODES_FILE);
    pthread_mutex_unlock(&file_lock);
}

// Return 1 if ip string equals "0.0.0.0"
static int ip_is_empty(const char *ip) {
    return (ip == NULL) || (strcmp(ip, "0.0.0.0") == 0) || (strlen(ip)==0);
}

// Portable method to get local IP (works without external commands).
// It creates a UDP socket and "connects" to a public IP and reads the local address.
// macOS-friendly IP detection:
// Prefer Wi-Fi interface (en0). If fails, fallback to socket trick.
static int detect_local_ip(char *out, int outlen) {

    // Try macOS Wi-Fi first
    FILE *fp = popen("ipconfig getifaddr en0 2>/dev/null", "r");
    if (fp) {
        char buf[64];
        if (fgets(buf, sizeof(buf), fp)) {
            // remove newline
            buf[strcspn(buf, "\n")] = 0;
            if (strlen(buf) > 0) {
                strncpy(out, buf, outlen - 1);
                pclose(fp);
                return 0;
            }
        }
        pclose(fp);
    }

    // fallback (Linux/Windows compatible) — connect() trick
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;

    struct sockaddr_in remote;
    memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET;
    remote.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);

    connect(s, (struct sockaddr*)&remote, sizeof(remote));

    struct sockaddr_in local;
    socklen_t len = sizeof(local);
    getsockname(s, (struct sockaddr*)&local, &len);
    close(s);

    const char *res = inet_ntop(AF_INET, &local.sin_addr, out, outlen);
    if (!res) return -1;

    return 0;
}


// Build sockaddr for node index; if ip is 0.0.0.0 we fallback to loopback
static void build_sockaddr_for_index(int idx, struct sockaddr_in *out) {
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(node_ports[idx]);
    const char *ip = nodes[idx].ip;
    if (ip_is_empty(ip)) ip = "127.0.0.1";
    out->sin_addr.s_addr = inet_addr(ip);
}

// -------------------- Network distribution --------------------

// Compose entire nodes.dat content into buffer
static int compose_nodes_file_contents(char *buf, int maxlen) {
    int off = 0;
    pthread_mutex_lock(&file_lock);
    for (int i = 0; i < NODES_COUNT; ++i) {
        int n = snprintf(buf + off, (size_t)maxlen - off, "%c %s\n", nodes[i].node, nodes[i].ip);
        if (n < 0 || off + n >= maxlen) break;
        off += n;
    }
    pthread_mutex_unlock(&file_lock);
    return off;
}

// Broadcast updated nodes.dat to other nodes by sending a UDP message:
// "[NODES_UPDATE]\n<file contents>"
static void broadcast_nodes_update(void) {
    char payload[1024];
    int written = snprintf(payload, sizeof(payload), "[NODES_UPDATE]\n");
    int used = written;
    // append file contents
    used += compose_nodes_file_contents(payload + used, (int)sizeof(payload) - used);
    // send to all nodes except ourselves (and loopback)
    for (int i = 0; i < NODES_COUNT; ++i) {
        if (nodes[i].node == local_node) continue;
        struct sockaddr_in dest;
        build_sockaddr_for_index(i, &dest);
        // if destination is loopback and nobody else is listening, still send (harmless)
        sendto(sockfd, payload, (size_t)used, 0, (struct sockaddr*)&dest, sizeof(dest));
    }
}

// When receiving a NODES_UPDATE packet - replace local nodes array and write file
static void handle_nodes_update(const char *payload) {
    // payload contains lines "A ip\nB ip\n..."
    // parse and update nodes[]
    pthread_mutex_lock(&file_lock);
    // create a temporary copy of current nodes
    struct NodeSlot tmp[NODES_COUNT];
    for (int i = 0; i < NODES_COUNT; ++i) {
        tmp[i].node = nodes[i].node;
        strncpy(tmp[i].ip, nodes[i].ip, sizeof(tmp[i].ip)-1);
    }
    // parse payload after the header
    const char *p = payload;
    if (strncmp(p, "[NODES_UPDATE]", 14) == 0) {
        p += 14;
    }
    // skip whitespace/newline
    while (*p == '\n' || *p == '\r' || isspace((unsigned char)*p)) p++;
    char line[MAX_LINE];
    int idx = 0;
    while (*p && idx < NODES_COUNT) {
        // read one line
        int i = 0;
        while (*p && *p != '\n' && i < MAX_LINE-1) {
            line[i++] = *p++;
        }
        if (*p == '\n') p++;
        line[i] = '\0';
        char nodech; char ipbuf[64];
        if (sscanf(line, " %c %63s", &nodech, ipbuf) == 2) {
            int j = node_index_from_letter(toupper(nodech));
            if (j >= 0 && j < NODES_COUNT) {
                strncpy(tmp[j].ip, ipbuf, sizeof(tmp[j].ip)-1);
            }
        }
        idx++;
    }
    // commit tmp -> nodes and write file
    for (int i = 0; i < NODES_COUNT; ++i) {
        strncpy(nodes[i].ip, tmp[i].ip, sizeof(nodes[i].ip)-1);
    }
    // save to file
    save_nodes_file();
    pthread_mutex_unlock(&file_lock);
}

// -------------------- Receiver thread --------------------

static void *receiver_thread_func(void *arg) {
    (void)arg;
    char buf[2048];
    while (running) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(sockfd, buf, sizeof(buf)-1, 0, (struct sockaddr*)&from, &flen);
        if (n <= 0) {
            if (errno == EINTR) continue;
            break;
        }
        buf[n] = '\0';
        // If it's nodes update
        if (strncmp(buf, "[NODES_UPDATE]", 14) == 0) {
            handle_nodes_update(buf);
            continue;
        }
        // Otherwise for now: ignore or integrate with messaging code
        // (The CLI will call backend_receive wrapper if you need to display)
    }
    return NULL;
}

// -------------------- Public API --------------------

// Initializes backend automatically: detect ip, update nodes.dat, broadcast changes.
// Returns 0 on success.
int backend_init_auto(void) {
    load_nodes_file();

    // detect local ip
    char ipbuf[64] = "0.0.0.0";
    if (detect_local_ip(ipbuf, sizeof(ipbuf)) != 0) {
        // fallback: try environment or loopback
        strncpy(ipbuf, "127.0.0.1", sizeof(ipbuf)-1);
    }

    // check if ip already present
    int found_idx = -1;
    for (int i = 0; i < NODES_COUNT; ++i) {
        if (strcmp(nodes[i].ip, ipbuf) == 0) {
            found_idx = i;
            break;
        }
    }

    if (found_idx >= 0) {
        local_node = nodes[found_idx].node;
        strncpy(local_ip, nodes[found_idx].ip, sizeof(local_ip)-1);
    } else {
        // find first empty slot (0.0.0.0) and occupy it
        int free_idx = -1;
        for (int i = 0; i < NODES_COUNT; ++i) {
            if (ip_is_empty(nodes[i].ip)) {
                free_idx = i;
                break;
            }
        }
        if (free_idx == -1) {
            fprintf(stderr, "No free node slots available (A-E full)\n");
            return -1;
        }
        strncpy(nodes[free_idx].ip, ipbuf, sizeof(nodes[free_idx].ip)-1);
        local_node = nodes[free_idx].node;
        strncpy(local_ip, ipbuf, sizeof(local_ip)-1);
        save_nodes_file();
        // After modifying local file broadcast update to others so they can sync
        // We'll open UDP socket below and then broadcast.
    }

    // create UDP socket and bind to our node's port
    int idx = node_index_from_letter(local_node);
    if (idx < 0) idx = 0;
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }

    // Allow reuse
    int yes = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    memset(&my_addr, 0, sizeof(my_addr));
    my_addr.sin_family = AF_INET;
    my_addr.sin_port = htons(node_ports[idx]);
    my_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sockfd, (struct sockaddr*)&my_addr, sizeof(my_addr)) < 0) {
        perror("bind");
        close(sockfd);
        return -1;
    }

    running = 1;

    // start receiver thread
    if (pthread_create(&receiver_thread, NULL, receiver_thread_func, NULL) != 0) {
        perror("pthread_create receiver");
        // not fatal
    }

    // If we newly occupied a slot (i.e., local_ip equals ipbuf and nodes file now contains it),
    // broadcast the nodes update so other nodes' nodes.dat get updated.
    broadcast_nodes_update();

    printf("\n==========================================\n");
    printf(" Auto-initialized node %c with IP %s (port %d)\n", local_node, local_ip, node_ports[idx]);
    printf(" nodes.dat loaded & broadcasted if changed.\n");
    printf("==========================================\n\n");

    return 0;
}

// Provide a wrapper to receive non-control messages (blocking). Returns bytes copied to outbuf or 0 for no user message.
int backend_receive(char *out, int max_len) {
    // Blocking receive - reuse recvfrom; however our receiver thread also listens.
    // For simplicity, here we do a blocking recvfrom to return application messages.
    if (sockfd < 0) return -1;
    char buf[2048];
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    ssize_t n = recvfrom(sockfd, buf, sizeof(buf)-1, 0, (struct sockaddr*)&from, &flen);
    if (n <= 0) return -1;
    buf[n] = '\0';
    if (strncmp(buf, "[NODES_UPDATE]", 14) == 0) {
        handle_nodes_update(buf);
        return 0; // not an app-level message
    }
    // copy up to max_len-1
    int tocopy = (int)strnlen(buf, (size_t)max_len-1);
    memcpy(out, buf, (size_t)tocopy);
    out[tocopy] = '\0';
    return tocopy;
}

// Send a user-level message to node letter 'to'
void backend_send_message(char to, const char *msg) {
    if (sockfd < 0) return;
    int idx = node_index_from_letter(to);
    if (idx < 0 || idx >= NODES_COUNT) return;
    struct sockaddr_in dest;
    build_sockaddr_for_index(idx, &dest);
    // message includes sender node for readability
    char packet[800];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char tstamp[32];
    snprintf(tstamp, sizeof(tstamp), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
    snprintf(packet, sizeof(packet), "[%c ➜ %c @%s]: %s", local_node, to, tstamp, msg);
    sendto(sockfd, packet, (size_t)strlen(packet), 0, (struct sockaddr*)&dest, sizeof(dest));
}

// Broadcast user-level message to all other nodes
void backend_broadcast(const char *msg) {
    if (sockfd < 0) return;
    char packet[800];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char tstamp[32];
    snprintf(tstamp, sizeof(tstamp), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
    snprintf(packet, sizeof(packet), "[%c ➜ ALL @%s]: %s", local_node, tstamp, msg);
    for (int i = 0; i < NODES_COUNT; ++i) {
        if (nodes[i].node == local_node) continue;
        struct sockaddr_in dest;
        build_sockaddr_for_index(i, &dest);
        sendto(sockfd, packet, (size_t)strlen(packet), 0, (struct sockaddr*)&dest, sizeof(dest));
    }
}

// Graceful shutdown - stop threads and optionally reset our node slot to 0.0.0.0
void backend_close(void) {
    running = 0;
    // close socket to break recv
    if (sockfd >= 0) close(sockfd);
    sockfd = -1;
    // join receiver thread
    pthread_cancel(receiver_thread);
    pthread_join(receiver_thread, NULL);
    // reset our slot in nodes.dat to 0.0.0.0 so next machine can take it
    int idx = node_index_from_letter(local_node);
    if (idx >= 0 && idx < NODES_COUNT) {
        strncpy(nodes[idx].ip, "0.0.0.0", sizeof(nodes[idx].ip)-1);
        save_nodes_file();
        // broadcast update so others sync
        // create a temporary socket to broadcast the change
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s >= 0) {
            // reuse existing method: send to known nodes
            char payload[1024];
            int written = snprintf(payload, sizeof(payload), "[NODES_UPDATE]\n");
            int used = written;
            used += compose_nodes_file_contents(payload + used, (int)sizeof(payload) - used);
            for (int i = 0; i < NODES_COUNT; ++i) {
                if (i == idx) continue;
                struct sockaddr_in dest;
                build_sockaddr_for_index(i, &dest);
                sendto(s, payload, (size_t)used, 0, (struct sockaddr*)&dest, sizeof(dest));
            }
            close(s);
        }
    }
    printf("Backend closed; nodes.dat updated (our slot reset).\n");
}
