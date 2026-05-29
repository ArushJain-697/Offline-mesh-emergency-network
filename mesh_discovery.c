#include "mesh_discovery.h"
#include "mesh_crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <errno.h>

static int disc_fd = -1;

/* ── Discovery socket ─────────────────────────────────────────── */

int discovery_init(void) {
    disc_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (disc_fd < 0) { perror("[DISCOVERY] socket"); return -1; }

    int opt = 1;
    setsockopt(disc_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(disc_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(DISCOVERY_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(disc_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[DISCOVERY] bind port 9000");
        close(disc_fd);
        disc_fd = -1;
        return -1;
    }
    printf("[DISCOVERY] Listening for peers on port %d\n", DISCOVERY_PORT);
    return disc_fd;
}

int discovery_get_fd(void) { return disc_fd; }

void discovery_close(void) {
    if (disc_fd >= 0) { close(disc_fd); disc_fd = -1; }
}

/* ── Own-IP detection ─────────────────────────────────────────── */

void discovery_get_my_ip(char *out, size_t len) {
    /* Connect a dummy UDP socket — no data is sent.
       The OS reveals which local interface it would use. */
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { strncpy(out, "127.0.0.1", len); return; }

    struct sockaddr_in tmp = {0};
    tmp.sin_family      = AF_INET;
    tmp.sin_port        = htons(DISCOVERY_PORT);
    inet_pton(AF_INET, "10.255.255.255", &tmp.sin_addr);
    connect(fd, (struct sockaddr *)&tmp, sizeof(tmp));

    struct sockaddr_in local = {0};
    socklen_t loc_len = sizeof(local);
    getsockname(fd, (struct sockaddr *)&local, &loc_len);
    close(fd);
    inet_ntop(AF_INET, &local.sin_addr, out, len);
}

/* ── Internal: send one discover packet and wait for welcome ──── */

/* Sends "NET_DISCOVER:<my_port>" from a temporary broadcast socket to
   target_ip:DISCOVERY_PORT, then waits up to timeout_ms for a
   NET_WELCOME reply on sock_fd (which is already bound to my_port).
   Returns malloc'd welcome string or NULL. */
static char *send_discover_and_wait(const char *target_ip, int my_port,
                                     int sock_fd, int timeout_ms)
{
    /* Temporary socket just for sending the broadcast */
    int bfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (bfd < 0) return NULL;

    int opt = 1;
    setsockopt(bfd, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

    char packet[64];
    snprintf(packet, sizeof(packet), "NET_DISCOVER:%d", my_port);

    struct sockaddr_in dest = {0};
    dest.sin_family      = AF_INET;
    dest.sin_port        = htons(DISCOVERY_PORT);
    inet_pton(AF_INET, target_ip, &dest.sin_addr);

    /* Encrypt before sending so only keyholders can parse our knock */
    uint8_t enc[256];
    int enc_len = crypto_encrypt_packet((const uint8_t *)packet, strlen(packet),
                                         enc, sizeof(enc));
    if (enc_len > 0)
        sendto(bfd, enc, enc_len, 0, (struct sockaddr *)&dest, sizeof(dest));
    close(bfd);

    /* Listen on the personal chat socket for NET_WELCOME */
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(sock_fd, &rfds);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };

    if (select(sock_fd + 1, &rfds, NULL, NULL, &tv) <= 0)
        return NULL;

    /* Decrypt the incoming NET_WELCOME */
    uint8_t raw[2048];
    int raw_n = recv(sock_fd, raw, sizeof(raw), 0);
    if (raw_n <= 0) return NULL;

    char *buf = malloc(2048);
    if (!buf) return NULL;
    int plain_n = crypto_decrypt_packet(raw, raw_n, (uint8_t *)buf, 2047);
    if (plain_n <= 0 || strncmp(buf, "NET_WELCOME:", 12) != 0) {
        free(buf);
        return NULL;
    }
    buf[plain_n] = '\0';
    return buf;
}

/* ── Public: 3-layer bootstrap ────────────────────────────────── */

char *discovery_bootstrap(int sock_fd, int my_port,
                           const char *helper_ip, int helper_port)
{
    char *welcome = NULL;
    (void)helper_port; /* port is encoded in the packet payload */

    /* Layer 1: Docker subnet broadcast (same-machine containers) */
    printf("[DISCOVERY] Layer 1: subnet broadcast (10.10.0.255)...\n");
    welcome = send_discover_and_wait("10.10.0.255", my_port, sock_fd, 1000);
    if (welcome) return welcome;

    /* Layer 2: LAN broadcast (WSL2 mirrored, native Linux) */
    printf("[DISCOVERY] Layer 2: LAN broadcast (255.255.255.255)...\n");
    welcome = send_discover_and_wait("255.255.255.255", my_port, sock_fd, 1000);
    if (welcome) return welcome;

    /* Layer 3: Targeted helper IP (cross-OS fallback) */
    if (helper_ip != NULL) {
        printf("[DISCOVERY] Layer 3: targeted helper at %s...\n", helper_ip);
        welcome = send_discover_and_wait(helper_ip, my_port, sock_fd, 2000);
        if (welcome) return welcome;
        fprintf(stderr, "[DISCOVERY] Targeted helper %s did not respond.\n",
                helper_ip);
    }

    /* Nobody answered */
    return NULL;
}

/* ── Public: handle an incoming NET_DISCOVER on disc_fd ────────── */

void discovery_handle_incoming(on_new_peer_fn on_new_peer) {
    uint8_t raw[256];
    struct sockaddr_in sender = {0};
    socklen_t slen = sizeof(sender);

    int raw_n = recvfrom(disc_fd, raw, sizeof(raw), 0,
                         (struct sockaddr *)&sender, &slen);
    if (raw_n <= 0) return;

    /* Decrypt — silently ignore packets from nodes without the key */
    char buf[128];
    int n = crypto_decrypt_packet(raw, raw_n, (uint8_t *)buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = '\0';

    /* Expect "NET_DISCOVER:<port>" */
    int new_port = 0;
    if (sscanf(buf, "NET_DISCOVER:%d", &new_port) != 1 || new_port <= 0)
        return;

    /* Extract sender IP from the recvfrom address */
    char new_ip[64];
    inet_ntop(AF_INET, &sender.sin_addr, new_ip, sizeof(new_ip));

    printf("[DISCOVERY] Peer knocking: %s:%d\n", new_ip, new_port);

    /* Delegate to mesh_backend via callback */
    if (on_new_peer) on_new_peer(new_ip, new_port);
}
