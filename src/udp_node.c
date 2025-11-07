// udp_node.c — only display incoming messages (no "You:" for self)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>

#define BUF 1024

static int MY_PORT;
static char TARGET_IP[64];
static int TARGET_PORT;
static char NODE_NAME[32];
static char PEER_NAME[32];

static int sockfd;
static struct sockaddr_in my_addr, peer_addr;

// Thread 1: receive messages only
void* rx_thread(void* arg) {
    char buf[BUF];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);

    while (1) {
        memset(buf, 0, sizeof(buf));
        ssize_t n = recvfrom(sockfd, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr*)&from, &fromlen);
        if (n > 0) {
            printf("\n💬 %s -> %s", PEER_NAME, buf);
            fflush(stdout);
        }
    }
    return NULL;
}

// Thread 2: send messages only (no echo)
void* tx_thread(void* arg) {
    char line[BUF];
    while (1) {
        if (!fgets(line, sizeof(line), stdin)) break;
        sendto(sockfd, line, strlen(line), 0,
               (struct sockaddr*)&peer_addr, sizeof(peer_addr));
    }
    return NULL;
}

int main(int argc, char** argv) {
    if (argc != 6) {
        fprintf(stderr,
        "Usage:\n./udp_node <my_port> <target_ip> <target_port> <my_name> <peer_name>\n");
        return 1;
    }

    MY_PORT = atoi(argv[1]);
    strncpy(TARGET_IP, argv[2], sizeof(TARGET_IP)-1);
    TARGET_PORT = atoi(argv[3]);
    strncpy(NODE_NAME, argv[4], sizeof(NODE_NAME)-1);
    strncpy(PEER_NAME, argv[5], sizeof(PEER_NAME)-1);

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);

    memset(&my_addr, 0, sizeof(my_addr));
    my_addr.sin_family = AF_INET;
    my_addr.sin_port = htons(MY_PORT);
    my_addr.sin_addr.s_addr = INADDR_ANY;
    bind(sockfd, (struct sockaddr*)&my_addr, sizeof(my_addr));

    memset(&peer_addr, 0, sizeof(peer_addr));
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_port = htons(TARGET_PORT);
    peer_addr.sin_addr.s_addr = inet_addr(TARGET_IP);

    printf("🟢 %s active. Listening on %d, sending to %s (%s:%d)\n",
           NODE_NAME, MY_PORT, PEER_NAME, TARGET_IP, TARGET_PORT);

    pthread_t rx, tx;
    pthread_create(&rx, NULL, rx_thread, NULL);
    pthread_create(&tx, NULL, tx_thread, NULL);

    pthread_join(rx, NULL);
    pthread_join(tx, NULL);
    close(sockfd);

    return 0;
}
