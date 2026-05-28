#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>
#define NODES_DATA_FILE "nodes.dat"
#define MAX_LINE 256
#define MAX_IP_LEN 64

struct Node
{
    char name;
    char ip[MAX_IP_LEN];
    int port;
};

void get_time_str(char *buf, size_t size)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(buf, size, "%H:%M:%S", t);
}

void log_to_history(char helper_node, const char *msg)
{
    char filename[64];
    snprintf(filename, sizeof(filename), "chat_history_%c.txt", helper_node);
    FILE *fp = fopen(filename, "a");
    if (fp)
    {
        fprintf(fp, "%s\n", msg);
        fclose(fp);
    }
}

int load_nodes(struct Node *nodes, int max_nodes)
{
    FILE *fp = fopen(NODES_DATA_FILE, "r");
    if (!fp)
        return 0;
    int count = 0;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), fp) != NULL && count < max_nodes)
    {
        char name;
        char ip[MAX_IP_LEN];
        int port;
        if (sscanf(line, " %c %63s %d", &name, ip, &port) == 3)
        {
            nodes[count].name = name;
            nodes[count].port = port;
            strncpy(nodes[count].ip, ip, MAX_IP_LEN - 1);
            nodes[count].ip[MAX_IP_LEN - 1] = '\0';
            count++;
        }
    }
    fclose(fp);
    return count;
}

int save_nodes(struct Node *nodes, int count)
{
    FILE *fp = fopen(NODES_DATA_FILE, "w");
    if (!fp)
    {
        perror("saving nodes.dat");
        return 0;
    }
    for (int i = 0; i < count; ++i)
    {
        fprintf(fp, "%c %s %d\n", nodes[i].name, nodes[i].ip, nodes[i].port);
    }
    fclose(fp);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 5)
    {
        fprintf(stderr, "Usage: %s <HelperNodeLetter> <NewNodeLetter> <NewNodeIP> <NewNodePort>\n", argv[0]);
        return 1;
    }
    char helper = toupper((unsigned char)argv[1][0]);
    char newn = toupper((unsigned char)argv[2][0]);
    char *newip = argv[3];
    int newport = atoi(argv[4]);

    struct Node nodes[26];
    int count = load_nodes(nodes, 26);

    int found = 0;
    for (int i = 0; i < count; ++i)
    {
        if (nodes[i].name == newn)
        {
            strncpy(nodes[i].ip, newip, MAX_IP_LEN - 1);
            nodes[i].ip[MAX_IP_LEN - 1] = '\0';
            nodes[i].port = newport;
            found = 1;
            break;
        }
    }
    if (!found)
    {
        if (count < 26)
        {
            nodes[count].name = newn;
            nodes[count].port = newport;
            strncpy(nodes[count].ip, newip, MAX_IP_LEN - 1);
            nodes[count].ip[MAX_IP_LEN - 1] = '\0';
            count++;
        }
        else
        {
            fprintf(stderr, "nodes.dat full, cannot add new node\n");
            return 1;
        }
    }

    if (!save_nodes(nodes, count))
    {
        fprintf(stderr, "Failed to save nodes.dat\n");
        return 1;
    }

    char payload[256];
    snprintf(payload, sizeof(payload), "NET_ADD:%c:%s:%d", newn, newip, newport);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        perror("socket");
        return 1;
    }

    for (int i = 0; i < count; ++i)
    {
        if (nodes[i].name == helper)
            continue;

        in_addr_t addr = inet_addr(nodes[i].ip);
        if (addr == INADDR_NONE)
        {
            fprintf(stderr, "Bad IP for node %c: %s, skipping\n", nodes[i].name, nodes[i].ip);
            continue;
        }

        struct sockaddr_in dest;
        memset(&dest, 0, sizeof(dest));
        dest.sin_family = AF_INET;
        dest.sin_port = htons(nodes[i].port);
        dest.sin_addr.s_addr = addr;

        ssize_t s = sendto(sock, payload, strlen(payload), 0,
                           (struct sockaddr *)&dest, sizeof(dest));
        if (s < 0)
            perror("sendto");
        else
            printf("Sent NET_ADD to %c @ %s:%d\n", nodes[i].name, nodes[i].ip, nodes[i].port);
    }

    close(sock);

    char time_str[16];
    get_time_str(time_str, sizeof(time_str));

    char log_entry[512];
    snprintf(log_entry, sizeof(log_entry),
             "[%s] [SYSTEM] new node %c has joined, its port is %d, ip %s",
             time_str, newn, newport, newip);

    log_to_history(helper, log_entry);

    printf("Helper update done. Added to local nodes.dat and history.\n");
    return 0;
}
