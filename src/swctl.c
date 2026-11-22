// swctl.c - admin tool for the switch
//
//   ./swctl show mac
//   ./swctl show ports
//   ./swctl clear mac
//   ./swctl shutdown <port>
//   ./swctl up <port>
//   ./swctl vlan <port> <vlan>
//   ./swctl mirror <port> <sniffer_port|off>
//   ./swctl stats [--watch]
//
// stats reads the shared memory directly, everything else is sent to
// the switch over the unix socket and the reply is printed

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "net_io.h"
#include "shm_stats.h"

static void usage(void)
{
    fprintf(stderr, "usage:\n"
                    "  swctl show mac\n"
                    "  swctl show ports\n"
                    "  swctl clear mac\n"
                    "  swctl shutdown <port>\n"
                    "  swctl up <port>\n"
                    "  swctl vlan <port> <vlan>\n"
                    "  swctl mirror <port> <sniffer_port|off>\n"
                    "  swctl stats [--watch]\n");
}

// sends one command line and prints whatever comes back
static int send_command(const char *cmd)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, ADMIN_SOCK, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is the switch running?)");
        close(fd);
        return 1;
    }

    char line[256];
    snprintf(line, sizeof(line), "%s\n", cmd);
    if (write_full(fd, line, strlen(line)) < 0) {
        perror("write");
        close(fd);
        return 1;
    }

    // the switch closes the connection when its done
    char buf[1024];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) != 0) {
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("read");
            break;
        }
        if (fwrite(buf, 1, n, stdout) != (size_t)n) {
            perror("stdout");
            break;
        }
    }
    if (close(fd) < 0)
        perror("close");
    return 0;
}

static void print_stats(struct shm_stats *s)
{
    struct port_stats ps[MAX_PORTS];
    int n = shm_stats_snapshot(s, ps);

    printf("%-5s %-10s %-10s %-10s %-10s\n", "PORT", "IN", "OUT", "DROPPED", "MIRRORED");
    for (int i = 0; i < n; i++) {
        printf("%-5d %-10lu %-10lu %-10lu %-10lu\n", i,
               ps[i].count[STAT_IN], ps[i].count[STAT_OUT],
               ps[i].count[STAT_DROPPED], ps[i].count[STAT_MIRRORED]);
    }
    fflush(stdout);
}

static int do_stats(int watch)
{
    struct shm_stats *s = shm_stats_attach();
    if (s == NULL) {
        fprintf(stderr, "could not open shared memory (is the switch running?)\n");
        return 1;
    }

    if (!watch) {
        print_stats(s);
        shm_stats_detach(s);
        return 0;
    }

    // redraw once a second until ctrl+c
    // the sleep only paces the screen, it isnt used to sync anything
    while (1) {
        printf("\033[H\033[J");  // clear the screen
        time_t now = time(NULL);
        printf("switch stats, %s", ctime(&now));
        print_stats(s);
        sleep(1);
    }
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage();
        return 1;
    }

    if (strcmp(argv[1], "stats") == 0) {
        if (argc == 2)
            return do_stats(0);
        if (argc == 3 && strcmp(argv[2], "--watch") == 0)
            return do_stats(1);
        usage();
        return 1;
    }

    // everything else goes to the switch as one line
    char cmd[256] = "";
    for (int i = 1; i < argc; i++) {
        if (strlen(cmd) + strlen(argv[i]) + 2 > sizeof(cmd)) {
            fprintf(stderr, "command too long\n");
            return 1;
        }
        if (i > 1)
            strcat(cmd, " ");
        strcat(cmd, argv[i]);
    }
    return send_command(cmd);
}
