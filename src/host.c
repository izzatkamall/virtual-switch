// host.c - a test host that connects to one port of the switch
//
//   ./host <switch_ip> <tcp_port> <mac> [vlan]
//
// type lines like:
//   02:00:00:00:00:02 hello     send to one host
//   bcast hello everyone        send to everyone
//   p0 02:00:00:00:00:02 hi     same but with priority 0, 1 or 2
// every frame we get is printed

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "frame.h"
#include "net_io.h"

static int sock;

static void *recv_thread(void *arg)
{
    (void)arg;
    struct frame_header h;
    uint8_t payload[FRAME_MAX_PAYLOAD + 1];

    while (1) {
        int r = frame_read(sock, &h, payload);
        if (r <= 0) {
            printf(r == 0 ? "switch closed the connection\n" : "connection error\n");
            fflush(stdout);
            break;
        }
        payload[h.length] = '\0';

        char src[MAC_STR_LEN];
        mac_to_string(h.src, src);
        printf("[from %s vlan %d prio %d] %s\n", src, h.vlan, h.priority, (char *)payload);
        fflush(stdout);
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc != 4 && argc != 5) {
        fprintf(stderr, "usage: %s <switch_ip> <tcp_port> <mac> [vlan]\n", argv[0]);
        return 1;
    }

    uint8_t my_mac[MAC_LEN];
    if (mac_from_string(argv[3], my_mac) != 0) {
        fprintf(stderr, "bad mac %s, use aa:bb:cc:dd:ee:ff\n", argv[3]);
        return 1;
    }
    int vlan = argc == 5 ? atoi(argv[4]) : 1;
    if (vlan < VLAN_MIN || vlan > VLAN_MAX) {
        fprintf(stderr, "vlan must be %d to %d\n", VLAN_MIN, VLAN_MAX);
        return 1;
    }

    // we handle EPIPE from write instead of dying
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)atoi(argv[2]));
    if (inet_pton(AF_INET, argv[1], &addr.sin_addr) != 1) {
        fprintf(stderr, "bad ip %s\n", argv[1]);
        return 1;
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }
    printf("connected as %s\n", argv[3]);
    fflush(stdout);

    pthread_t tid;
    if (pthread_create(&tid, NULL, recv_thread, NULL) != 0) {
        fprintf(stderr, "could not start receive thread\n");
        close(sock);
        return 1;
    }

    char line[FRAME_MAX_PAYLOAD + 64];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        line[strcspn(line, "\n")] = '\0';
        char *p = line;

        struct frame_header h;
        memset(&h, 0, sizeof(h));
        h.vlan = (uint16_t)vlan;
        h.priority = PRIO_NORMAL;
        memcpy(h.src, my_mac, MAC_LEN);

        // optional priority like p0
        if (p[0] == 'p' && p[1] >= '0' && p[1] <= '2' && p[2] == ' ') {
            h.priority = (uint8_t)(p[1] - '0');
            p += 3;
        }

        // first word is where it goes
        char *dst = strtok(p, " ");
        if (dst == NULL)
            continue;
        char *text = strtok(NULL, "");
        if (text == NULL)
            text = "";

        if (strcmp(dst, "bcast") == 0) {
            memcpy(h.dst, MAC_BROADCAST, MAC_LEN);
        } else if (mac_from_string(dst, h.dst) != 0) {
            printf("bad dest mac %s\n", dst);
            continue;
        }

        size_t len = strlen(text);
        if (len > FRAME_MAX_PAYLOAD)
            len = FRAME_MAX_PAYLOAD;
        h.length = (uint16_t)len;

        if (frame_write(sock, &h, (uint8_t *)text) < 0) {
            perror("send");
            break;
        }
    }

    // stdin is done, tell the switch we wont send more but keep reading
    // until it closes, so replies still get printed
    if (shutdown(sock, SHUT_WR) < 0)
        perror("shutdown");
    if (pthread_join(tid, NULL) != 0)
        fprintf(stderr, "could not join receive thread\n");
    if (close(sock) < 0)
        perror("close");
    return 0;
}
