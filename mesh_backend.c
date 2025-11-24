// mesh_backend.c (MacBook Fixed Version)
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

// ---- Utilities ----
static int node_index_from_letter(char node) {
    if (node < 'A' || node > 'E') return -1;
    return node - 'A';
}

static int ip_is_empty(const char *ip) {
    return (ip == NULL) || (strcmp(ip, "0.0.0.0") == 0) || (strlen(ip)==0);
}

static void init_nodes_default(void) {
    for (int i = 0; i < NODES_COUNT; ++i) {
        nodes[i].node = 'A' + i;
        strncpy(nodes[i].ip, "0.0.0.0", sizeof(nodes[i].ip)-1);
    }
}

static void load_nodes_file(void) {
    pthread_mutex_lock(&file_lock);
    FILE *f = fopen(NODES_FILE, "r");
    init_nodes_default();
    
    if (!f) {
        FILE *g = fopen(NODES_FILE, "w");
        if (g) {
            for (int i = 0; i < NODES_COUNT; ++i) fprintf(g, "%c %s\n", nodes[i].node, nodes[i].ip);
            fclose(g);
        }
        pthread_mutex_unlock(&file_lock);
        return;
    }

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        char nodech; char ipbuf[64];
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

static void save_nodes_file(void) {
    pthread_mutex_lock(&file_lock);
    char tmpname[64];
    snprintf(tmpname, sizeof(tmpname), "%s.tmp", NODES_FILE);
    FILE *f = fopen(tmpname, "w");
    if (!f) { pthread_mutex_unlock(&file_lock); return; }
    for (int i = 0; i < NODES_COUNT; ++i) {
        fprintf(f, "%c %s\n", nodes[i].node, nodes[i].ip);
    }
    fclose(f);
    rename(tmpname, NODES_FILE);
    pthread_mutex_unlock(&file_lock);
}

static void build_sockaddr_for_index(int idx, struct sockaddr_in *out, int use_broadcast_if_empty) {
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(node_ports[idx]);
    const char *ip = nodes[idx].ip;
    if (ip_is_empty(ip)) {
        if (use_broadcast_if_empty) out->sin_addr.s_addr = htonl(INADDR_BROADCAST);
        else out->sin_addr.s_addr = inet_addr("127.0.0.1");
    } else {
        out->sin_addr.s_addr = inet_addr(ip);
    }
}

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

static void broadcast_nodes_update(void) {
    char payload[1024];
    int written = snprintf(payload, sizeof(payload), "[NODES_UPDATE]\n");
    int used = written + compose_nodes_file_contents(payload + written, (int)sizeof(payload) - written);

    for (int i = 0; i < NODES_COUNT; ++i) {
        if (nodes[i].node == local_node) continue;
        if (!ip_is_empty(nodes[i].ip)) {
            struct sockaddr_in dest;
            build_sockaddr_for_index(i, &dest, 0);
            sendto(sockfd, payload, (size_t)used, 0, (struct sockaddr*)&dest, sizeof(dest));
        }
    }

    struct sockaddr_in bdest;
    memset(&bdest, 0, sizeof(bdest));
    bdest.sin_family = AF_INET;
    bdest.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    for (int i = 0; i < NODES_COUNT; ++i) {
        bdest.sin_port = htons(node_ports[i]);
        sendto(sockfd, payload, (size_t)used, 0, (struct sockaddr*)&bdest, sizeof(bdest));
    }
}

static void handle_nodes_update(const char *payload) {
    pthread_mutex_lock(&file_lock);
    struct NodeSlot tmp[NODES_COUNT];
    for(int i=0; i<NODES_COUNT; i++) tmp[i] = nodes[i];
    
    const char *p = strstr(payload, "[NODES_UPDATE]");
    if (p) p += 14; else p = payload;
    
    char line[MAX_LINE];
    while (*p) {
        int i=0;
        while(*p && *p!='\n' && i<MAX_LINE-1) line[i++] = *p++;
        if(*p=='\n') p++;
        line[i] = '\0';
        
        char n; char ip[64];
        if (sscanf(line, " %c %63s", &n, ip) == 2) {
            int idx = node_index_from_letter(toupper(n));
            if (idx >= 0 && idx < NODES_COUNT && !ip_is_empty(ip)) {
                strncpy(tmp[idx].ip, ip, sizeof(tmp[idx].ip)-1);
            }
        }
    }
    
    for(int i=0; i<NODES_COUNT; i++) strncpy(nodes[i].ip, tmp[i].ip, sizeof(nodes[i].ip)-1);
    save_nodes_file();
    pthread_mutex_unlock(&file_lock);
}

// Receiver Thread (Fix: Handle shutdown gracefully)
static void *receiver_thread_func(void *arg) {
    (void)arg;
    char buf[2048];
    while (running) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(sockfd, buf, sizeof(buf)-1, 0, (struct sockaddr*)&from, &flen);
        
        if (n <= 0) {
            // If stopped, break immediately
            if (!running) break;
            continue;
        }
        buf[n] = '\0';

        if (strncmp(buf, "[NODES_UPDATE]", 14) == 0) {
            handle_nodes_update(buf);
            continue; 
        }
    }
    return NULL;
}

// ---- Public API ----

// IP Detection (Fix: Added macOS specific 'en0' check)
static int detect_local_ip(char *out, int outlen) {
    // 1. Try macOS command (Best for MacBook Air)
    FILE *fp = popen("ipconfig getifaddr en0 2>/dev/null", "r");
    if (fp) {
        char buf[64];
        if (fgets(buf, sizeof(buf), fp)) {
            size_t len = strlen(buf);
            if (len > 0 && buf[len-1] == '\n') buf[len-1] = 0;
            if (strlen(buf) > 0) {
                strncpy(out, buf, outlen - 1);
                pclose(fp);
                return 0;
            }
        }
        pclose(fp);
    }

    // 2. Fallback: Google DNS trick (for others or if en0 fails)
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in remote;
    memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET;
    remote.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);
    if (connect(s, (struct sockaddr*)&remote, sizeof(remote)) < 0) {
        close(s); return -1;
    }
    struct sockaddr_in local;
    socklen_t len = sizeof(local);
    getsockname(s, (struct sockaddr*)&local, &len);
    close(s);
    if (inet_ntop(AF_INET, &local.sin_addr, out, outlen) == NULL) return -1;
    return 0;
}

int backend_init_auto(void) {
    load_nodes_file();
    
    char ipbuf[64] = "0.0.0.0";
    if (detect_local_ip(ipbuf, sizeof(ipbuf)) != 0) {
        printf("[WARN] Could not detect LAN IP, using 127.0.0.1\n");
        strncpy(ipbuf, "127.0.0.1", sizeof(ipbuf)-1);
    }

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
        int free_idx = -1;
        for (int i = 0; i < NODES_COUNT; ++i) {
            if (ip_is_empty(nodes[i].ip)) { free_idx = i; break; }
        }
        if (free_idx == -1) {
            fprintf(stderr, "Error: Mesh is full (Nodes A-E taken).\n");
            return -1;
        }
        local_node = nodes[free_idx].node;
        strncpy(nodes[free_idx].ip, ipbuf, sizeof(nodes[free_idx].ip)-1);
        strncpy(local_ip, ipbuf, sizeof(local_ip)-1);
        save_nodes_file();
    }

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    int yes=1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    int idx = node_index_from_letter(local_node);
    memset(&my_addr, 0, sizeof(my_addr));
    my_addr.sin_family = AF_INET;
    my_addr.sin_port = htons(node_ports[idx]);
    my_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sockfd, (struct sockaddr*)&my_addr, sizeof(my_addr)) < 0) {
        perror("Bind failed");
        return -1;
    }

    running = 1;
    pthread_create(&receiver_thread, NULL, receiver_thread_func, NULL);
    broadcast_nodes_update();
    
    printf("Initialized as Node %c (%s) on Port %d\n", local_node, local_ip, node_ports[idx]);
    return 0;
}

char backend_get_local_node(void) { return local_node; }

void backend_force_sync(void) {
    if (!running) return;
    broadcast_nodes_update();
}

int backend_receive(char *out, int max_len) {
    if (sockfd < 0) return -1;
    char buf[2048];
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    ssize_t n = recvfrom(sockfd, buf, sizeof(buf)-1, 0, (struct sockaddr*)&from, &flen);
    if (n <= 0) return -1;
    buf[n] = '\0';
    
    if (strncmp(buf, "[NODES_UPDATE]", 14) == 0) {
        handle_nodes_update(buf);
        return 0; 
    }
    
    strncpy(out, buf, max_len);
    out[max_len-1] = 0;
    return strlen(out);
}

void backend_send_message(char to, const char *msg) {
    int idx = node_index_from_letter(to);
    if (idx < 0) return;
    struct sockaddr_in dest;
    build_sockaddr_for_index(idx, &dest, 0);
    
    char packet[800];
    snprintf(packet, sizeof(packet), "[%c]: %s", local_node, msg);
    sendto(sockfd, packet, strlen(packet), 0, (struct sockaddr*)&dest, sizeof(dest));
}

void backend_broadcast(const char *msg) {
    char packet[800];
    snprintf(packet, sizeof(packet), "[%c->ALL]: %s", local_node, msg);
    for (int i = 0; i < NODES_COUNT; ++i) {
        if (nodes[i].node == local_node) continue;
        struct sockaddr_in dest;
        build_sockaddr_for_index(i, &dest, 1); 
        sendto(sockfd, packet, strlen(packet), 0, (struct sockaddr*)&dest, sizeof(dest));
    }
}

void backend_close(void) {
    running = 0;
    
    // Fix: Shutdown socket specifically to wake up blocked recvfrom
    if (sockfd >= 0) {
        shutdown(sockfd, SHUT_RDWR);
        close(sockfd);
    }
    
    // Wait for thread to finish cleanly
    pthread_join(receiver_thread, NULL);
    sockfd = -1;

    int idx = node_index_from_letter(local_node);
    if (idx >= 0) {
        strncpy(nodes[idx].ip, "0.0.0.0", 63);
        save_nodes_file();
        
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        int yes=1;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
        
        char payload[1024];
        int w = snprintf(payload, sizeof(payload), "[NODES_UPDATE]\n");
        w += compose_nodes_file_contents(payload+w, sizeof(payload)-w);
        
        struct sockaddr_in bdest;
        bdest.sin_family = AF_INET;
        bdest.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        for(int i=0; i<NODES_COUNT; i++) {
            bdest.sin_port = htons(node_ports[i]);
            sendto(s, payload, w, 0, (struct sockaddr*)&bdest, sizeof(bdest));
        }
        close(s);
    }
}
