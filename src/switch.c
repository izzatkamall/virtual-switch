// switch.c - the virtual switch
//
//   ./switch <tcp_port> <num_ports> [config_file]
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
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "frame.h"
#include "log.h"
#include "mac_table.h"
#include "net_io.h"

#define QUEUE_LEN 64  // frames waiting per port
#define DUMP_FILE "mac_dump.txt"  // SIGUSR1 writes the mac table here
#define LOG_FILE "switch.log"

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
static sem_t free_ports;  // counts ports nobody is using
static int listen_fd = -1;

// settings from the config file, SIGHUP can change them while we run
static pthread_mutex_t cfg_lock = PTHREAD_MUTEX_INITIALIZER;
static struct config cfg;
static const char *cfg_path = NULL;

static struct config get_config(void)
{
    pthread_mutex_lock(&cfg_lock);
    struct config c = cfg;
    pthread_mutex_unlock(&cfg_lock);
    return c;
}

// loads the config file and puts the new values into use
static int apply_config(void)
{
    struct config c = get_config();
    if (cfg_path != NULL && config_load(cfg_path, &c) != 0)
        return -1;

    pthread_mutex_lock(&cfg_lock);
    cfg = c;
    pthread_mutex_unlock(&cfg_lock);

    mac_table_set_limits(&table, c.mac_max, c.mac_per_port);
    mac_table_trim(&table);  // in case mac_max got smaller
    return 0;
}

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

// returns -1 if the port broke port security and has to be shut
static int forward(int in_port, const struct frame_header *h, const uint8_t *payload)
{
    if (mac_table_insert(&table, h->src, in_port) == MAC_PORT_FULL) {
        log_msg("port %d has more than %d macs, shutting it down", in_port, get_config().mac_per_port);
        return -1;
    }

    int out = -1;
    if (!mac_is_broadcast(h->dst))
        out = mac_table_lookup(&table, h->dst);

    if (out == in_port)
        return 0;  // dest is on the same port, nothing to do

    // one copy no matter how many ports it goes to
    struct pkt *k = pkt_new(h, payload);
    if (k == NULL)
        return 0;

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
    return 0;
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

    while (frame_read(fd, &h, payload) == 1) {
        if (forward(pt->id, &h, payload) < 0)
            break;
    }

    // forget the macs on this port so a new host starts clean
    mac_table_remove_port(&table, pt->id);

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

    // one more port free, wakes up a host waiting in main
    sem_post(&free_ports);

    log_msg("port %d disconnected (%lu frames dropped)", pt->id, dropped);
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

static int is_stopping(void)
{
    pthread_mutex_lock(&stop_lock);
    int s = stopping;
    pthread_mutex_unlock(&stop_lock);
    return s;
}

static void dump_mac_table(void)
{
    char *text = mac_table_to_string(&table);
    if (text == NULL) {
        log_msg("could not build mac table dump");
        return;
    }
    FILE *f = fopen(DUMP_FILE, "w");
    if (f == NULL) {
        log_msg("fopen %s: %s", DUMP_FILE, strerror(errno));
        free(text);
        return;
    }
    fputs(text, f);
    if (fclose(f) != 0)
        log_msg("fclose %s: %s", DUMP_FILE, strerror(errno));
    free(text);
    log_msg("mac table dumped to %s", DUMP_FILE);
}

// the only thread that gets SIGINT, SIGHUP and SIGUSR1
// every other thread has them blocked
static void *signal_thread(void *arg)
{
    sigset_t *set = arg;

    while (1) {
        int sig;
        if (sigwait(set, &sig) != 0) {
            log_msg("sigwait failed");
            continue;
        }

        if (sig == SIGUSR1) {
            dump_mac_table();
        } else if (sig == SIGHUP) {
            if (cfg_path == NULL) {
                log_msg("got SIGHUP but there is no config file");
            } else if (apply_config() == 0) {
                struct config c = get_config();
                log_msg("reloaded %s: mac_age %d mac_max %d mac_per_port %d port_wait %d",
                       cfg_path, c.mac_age, c.mac_max, c.mac_per_port, c.port_wait);
            } else {
                log_msg("config has errors, keeping the old settings");
            }
        } else if (sig == SIGINT || sig == SIGTERM) {
            log_msg("shutting down");

            pthread_mutex_lock(&stop_lock);
            stopping = 1;
            pthread_cond_broadcast(&stop_cond);  // wakes the aging thread
            pthread_mutex_unlock(&stop_lock);

            // wake main if its stuck in accept or waiting for a port
            shutdown(listen_fd, SHUT_RDWR);
            sem_post(&free_ports);
            break;
        }
    }
    return NULL;
}

// kicks every host off, the rx threads then clean up like a normal disconnect
// only main calls this, after it stopped accepting, so no new port can show up
static void close_all_ports(void)
{
    for (int p = 0; p < num_ports; p++) {
        struct port *pt = &ports[p];
        pthread_mutex_lock(&pt->lock);
        if (pt->state == PORT_UP)
            shutdown(pt->fd, SHUT_RDWR);
        int started = pt->state != PORT_FREE;
        pthread_mutex_unlock(&pt->lock);

        // every port that isnt FREE has an rx thread nobody joined yet
        if (started) {
            pthread_join(pt->rx_tid, NULL);
            pt->state = PORT_FREE;
        }
    }
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
        int n = mac_table_age(&table, (uint64_t)get_config().mac_age * 1000);
        if (n > 0) {
            log_msg("aged out %d mac entries", n);
        }
    }
    return NULL;
}

// waits for a free port, returns 0 if we got one, -1 on timeout
static int wait_for_port(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += get_config().port_wait;

    while (sem_timedwait(&free_ports, &ts) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return 0;
}

// tells a host there is no room, as a normal frame so test hosts can read it
static void send_switch_full(int fd)
{
    struct frame_header h;
    memset(&h, 0, sizeof(h));
    memcpy(h.dst, MAC_BROADCAST, MAC_LEN);
    h.vlan = 1;
    h.priority = PRIO_HIGH;
    h.length = 11;
    frame_write(fd, &h, (const uint8_t *)"switch full");
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
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: %s <tcp_port> <num_ports> [config_file]\n", argv[0]);
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

    // block these before any thread starts so every thread inherits it,
    // then only the signal thread sees them through sigwait
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGUSR1);
    if (pthread_sigmask(SIG_BLOCK, &set, NULL) != 0) {
        fprintf(stderr, "pthread_sigmask failed\n");
        return 1;
    }

    // logger thread, started after the mask so it doesnt get signals either
    if (log_start(LOG_FILE) != 0)
        return 1;

    // a host leaving should give us EPIPE, not kill the switch
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        return 1;
    }

    if (mac_table_init(&table) != 0) {
        fprintf(stderr, "mac_table_init failed\n");
        return 1;
    }
    config_defaults(&cfg);
    if (argc == 4)
        cfg_path = argv[3];
    if (apply_config() != 0) {
        fprintf(stderr, "could not load config %s\n", cfg_path);
        mac_table_destroy(&table);
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

    if (sem_init(&free_ports, 0, (unsigned int)num_ports) != 0) {
        perror("sem_init");
        return 1;
    }

    listen_fd = make_listen_socket(tcp_port);
    if (listen_fd < 0)
        return 1;

    pthread_t sig_tid;
    if (pthread_create(&sig_tid, NULL, signal_thread, &set) != 0) {
        fprintf(stderr, "could not start signal thread\n");
        return 1;
    }

    pthread_t aging_tid;
    if (pthread_create(&aging_tid, NULL, aging_thread, NULL) != 0) {
        fprintf(stderr, "could not start aging thread\n");
        return 1;
    }

    log_msg("switch listening on port %d with %d ports", tcp_port, num_ports);

    while (!is_stopping()) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (is_stopping())
                break;
            if (errno == EINTR)
                continue;
            log_msg("accept: %s", strerror(errno));
            break;
        }

        // all ports busy, wait a bit for one to free up
        int got_port = wait_for_port();
        if (is_stopping()) {
            close(fd);
            break;
        }
        if (got_port < 0) {
            log_msg("switch full, sending a new host away");
            send_switch_full(fd);
            close(fd);
            continue;
        }

        int p = attach_host(fd);
        if (p < 0) {
            log_msg("could not attach host");
            sem_post(&free_ports);  // give the slot back
            close(fd);
        } else {
            log_msg("host connected on port %d", p);
        }
    }

    // if accept failed for some other reason, still shut down properly
    pthread_mutex_lock(&stop_lock);
    stopping = 1;
    pthread_cond_broadcast(&stop_cond);
    pthread_mutex_unlock(&stop_lock);

    close_all_ports();
    pthread_join(aging_tid, NULL);

    // signal thread might still be in sigwait if we got here without SIGINT
    pthread_kill(sig_tid, SIGINT);
    pthread_join(sig_tid, NULL);

    // nothing else is running now, free everything
    close(listen_fd);
    for (int p = 0; p < num_ports; p++) {
        pthread_mutex_destroy(&ports[p].lock);
        pthread_cond_destroy(&ports[p].not_empty);
    }
    sem_destroy(&free_ports);
    mac_table_destroy(&table);
    log_msg("switch stopped cleanly");
    log_stop();
    return 0;
}
