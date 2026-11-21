// switch.c - the virtual switch
//
//   ./switch <tcp_port> <num_ports>
//
// every host that connects gets a port with two threads
//   rx  reads frames, learns the source mac, puts the frame on the out queues
//   tx  takes frames off its own queue and writes them to the host
// a slow host only fills up its own queue, frames get dropped when its full

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "frame.h"
#include "mac_table.h"
#include "net_io.h"

#define MAX_PORTS 64
#define QUEUE_LEN 64  // frames waiting per port
#define MAC_AGE_SECS 30  // forget a mac not seen for this long

// one received frame, shared by every queue it goes into
// refs counts how many queues still hold it, the last one frees it
struct pkt {
    atomic_int refs;
    struct frame_header h;
    uint8_t payload[FRAME_MAX_PAYLOAD];
};

static struct pkt *pkt_new(const struct frame_header *h, const uint8_t *payload)
{
    struct pkt *k = malloc(sizeof(*k));
    if (k == NULL)
        return NULL;
    atomic_init(&k->refs, 1);
    k->h = *h;
    memcpy(k->payload, payload, h->length);
    return k;
}

static void pkt_get(struct pkt *k)
{
    atomic_fetch_add(&k->refs, 1);
}

// drops one reference, frees it if that was the last one
static void pkt_put(struct pkt *k)
{
    if (atomic_fetch_sub(&k->refs, 1) == 1)
        free(k);
}

// FREE: nobody here, UP: host connected, CLOSING: host left and
// threads are stopping, DONE: threads stopped, rx needs a join
enum { PORT_FREE, PORT_UP, PORT_CLOSING, PORT_DONE };

struct port {
    int id;
    pthread_mutex_t lock;  // protects everything below
    pthread_cond_t not_empty;
    int fd;
    int state;
    pthread_t rx_tid;
    pthread_t tx_tid;

    // ring buffer of frames to send out this port
    struct pkt *queue[QUEUE_LEN];
    int head;
    int count;
    unsigned long dropped;
};

static struct port ports[MAX_PORTS];
static int num_ports;
static struct mac_table table;

// set when the switch is shutting down, the cond wakes sleeping threads
static pthread_mutex_t stop_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t stop_cond = PTHREAD_COND_INITIALIZER;
static int stopping = 0;

// puts a frame on a port queue, never blocks
// if the queue is full the frame is dropped and counted
static void port_enqueue(int p, struct pkt *k)
{
    struct port *pt = &ports[p];

    pthread_mutex_lock(&pt->lock);
    if (pt->state != PORT_UP) {
        pthread_mutex_unlock(&pt->lock);
        return;
    }
    if (pt->count == QUEUE_LEN) {
        pt->dropped++;
        pthread_mutex_unlock(&pt->lock);
        return;
    }
    pkt_get(k);  // the queue holds a reference now
    pt->queue[(pt->head + pt->count) % QUEUE_LEN] = k;
    pt->count++;
    pthread_cond_signal(&pt->not_empty);
    pthread_mutex_unlock(&pt->lock);
}

static void forward(int in_port, const struct frame_header *h, const uint8_t *payload)
{
    mac_table_insert(&table, h->src, in_port);

    int out = -1;
    if (!mac_is_broadcast(h->dst))
        out = mac_table_lookup(&table, h->dst);

    if (out == in_port)
        return;  // dest is on the same port, nothing to do

    // one copy no matter how many ports it goes to
    struct pkt *k = pkt_new(h, payload);
    if (k == NULL)
        return;

    if (out >= 0) {
        port_enqueue(out, k);
    } else {
        // broadcast or unknown dest, send to everyone else
        for (int p = 0; p < num_ports; p++) {
            if (p != in_port)
                port_enqueue(p, k);
        }
    }

    // let go of our own reference, frees it if no queue took it
    pkt_put(k);
}

// takes frames off the queue and writes them to the host
// if this host is slow only this thread waits, nobody else
static void *tx_thread(void *arg)
{
    struct port *pt = arg;
    int fd = pt->fd;  // set before the thread started, never changes while we run

    while (1) {
        pthread_mutex_lock(&pt->lock);
        while (pt->count == 0 && pt->state == PORT_UP)
            pthread_cond_wait(&pt->not_empty, &pt->lock);
        if (pt->state != PORT_UP) {
            pthread_mutex_unlock(&pt->lock);
            break;
        }
        struct pkt *k = pt->queue[pt->head];
        pt->head = (pt->head + 1) % QUEUE_LEN;
        pt->count--;
        pthread_mutex_unlock(&pt->lock);

        int rc = frame_write(fd, &k->h, k->payload);
        pkt_put(k);
        if (rc < 0) {
            // host is gone, wake up the rx thread so it cleans up
            shutdown(fd, SHUT_RDWR);
            break;
        }
    }
    return NULL;
}

static void *rx_thread(void *arg)
{
    struct port *pt = arg;
    struct frame_header h;
    uint8_t payload[FRAME_MAX_PAYLOAD];
    int fd = pt->fd;

    while (frame_read(fd, &h, payload) == 1)
        forward(pt->id, &h, payload);

    // stop the sender, shutdown wakes it if its stuck in write()
    pthread_mutex_lock(&pt->lock);
    pt->state = PORT_CLOSING;
    pthread_cond_broadcast(&pt->not_empty);
    pthread_mutex_unlock(&pt->lock);
    shutdown(fd, SHUT_RDWR);
    pthread_join(pt->tx_tid, NULL);

    // both threads are done with the fd now
    pthread_mutex_lock(&pt->lock);
    while (pt->count > 0) {
        pkt_put(pt->queue[pt->head]);
        pt->head = (pt->head + 1) % QUEUE_LEN;
        pt->count--;
    }
    unsigned long dropped = pt->dropped;
    close(pt->fd);
    pt->fd = -1;
    pt->state = PORT_DONE;
    pthread_mutex_unlock(&pt->lock);

    printf("port %d disconnected (%lu frames dropped)\n", pt->id, dropped);
    fflush(stdout);
    return NULL;
}

// finds a free port and starts its threads, returns the port or -1
static int attach_host(int fd)
{
    for (int p = 0; p < num_ports; p++) {
        struct port *pt = &ports[p];

        pthread_mutex_lock(&pt->lock);
        int state = pt->state;
        pthread_mutex_unlock(&pt->lock);

        if (state == PORT_UP || state == PORT_CLOSING)
            continue;

        // old receiver already finished, join it before reusing the port
        if (state == PORT_DONE)
            pthread_join(pt->rx_tid, NULL);

        pthread_mutex_lock(&pt->lock);
        pt->fd = fd;
        pt->state = PORT_UP;
        pt->head = 0;
        pt->count = 0;
        pt->dropped = 0;
        pthread_mutex_unlock(&pt->lock);

        // sender first because rx joins it when the host leaves
        if (pthread_create(&pt->tx_tid, NULL, tx_thread, pt) != 0) {
            pthread_mutex_lock(&pt->lock);
            pt->fd = -1;
            pt->state = PORT_FREE;
            pthread_mutex_unlock(&pt->lock);
            return -1;
        }
        if (pthread_create(&pt->rx_tid, NULL, rx_thread, pt) != 0) {
            // stop the sender we just made
            pthread_mutex_lock(&pt->lock);
            pt->state = PORT_FREE;
            pt->fd = -1;
            pthread_cond_broadcast(&pt->not_empty);
            pthread_mutex_unlock(&pt->lock);
            pthread_join(pt->tx_tid, NULL);
            return -1;
        }
        return p;
    }
    return -1;
}

// waits about one second, returns 1 if we should stop
static int wait_one_second(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1;

    pthread_mutex_lock(&stop_lock);
    while (!stopping) {
        if (pthread_cond_timedwait(&stop_cond, &stop_lock, &ts) == ETIMEDOUT)
            break;
    }
    int s = stopping;
    pthread_mutex_unlock(&stop_lock);
    return s;
}

// removes old mac entries once a second
static void *aging_thread(void *arg)
{
    (void)arg;
    while (!wait_one_second()) {
        int n = mac_table_age(&table, MAC_AGE_SECS * 1000);
        if (n > 0) {
            printf("aged out %d mac entries\n", n);
            fflush(stdout);
        }
    }
    return NULL;
}

static int make_listen_socket(int tcp_port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0)
        perror("setsockopt");

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)tcp_port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 16) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <tcp_port> <num_ports>\n", argv[0]);
        return 1;
    }
    int tcp_port = atoi(argv[1]);
    num_ports = atoi(argv[2]);
    if (tcp_port < 1 || tcp_port > 65535) {
        fprintf(stderr, "bad tcp port\n");
        return 1;
    }
    if (num_ports < 1 || num_ports > MAX_PORTS) {
        fprintf(stderr, "num_ports must be 1 to %d\n", MAX_PORTS);
        return 1;
    }

    // a host leaving should give us EPIPE, not kill the switch
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        return 1;
    }

    if (mac_table_init(&table) != 0) {
        fprintf(stderr, "mac_table_init failed\n");
        return 1;
    }
    for (int p = 0; p < num_ports; p++) {
        ports[p].id = p;
        ports[p].fd = -1;
        ports[p].state = PORT_FREE;
        if (pthread_mutex_init(&ports[p].lock, NULL) != 0 ||
            pthread_cond_init(&ports[p].not_empty, NULL) != 0) {
            fprintf(stderr, "port init failed\n");
            return 1;
        }
    }

    int listen_fd = make_listen_socket(tcp_port);
    if (listen_fd < 0)
        return 1;

    pthread_t aging_tid;
    if (pthread_create(&aging_tid, NULL, aging_thread, NULL) != 0) {
        fprintf(stderr, "could not start aging thread\n");
        return 1;
    }

    printf("switch listening on port %d with %d ports\n", tcp_port, num_ports);
    fflush(stdout);

    while (1) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            break;
        }

        int p = attach_host(fd);
        if (p < 0) {
            // no free port, the semaphore version comes later
            printf("no free port, dropping new host\n");
            close(fd);
        } else {
            printf("host connected on port %d\n", p);
        }
        fflush(stdout);
    }

    close(listen_fd);
    mac_table_destroy(&table);
    return 0;
}
